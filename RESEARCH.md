# tinyuwb: Phase 0 research

A DW3000-family driver and FiRa/CCC responder ranging engine for
tinylock, replacing ultrawidelock's vendored Qorvo driver and its UWB engine.
Written 2026-10-18 from the sizes measured that day; nothing here is built
yet.

## Summary

| Question | Answer |
|---|---|
| UWB today in ultrawidelock (release, LTO, measured 2026-10-18) | 35,094 B flash, 4,211 B RAM |
| Of which the Qorvo driver and its glue | 22,139 B flash, 2,710 B RAM |
| Of which the engine (CCC shim, FiRa session, DS-TWR, diagnostics) | 12,955 B flash, 1,501 B RAM |
| Target | 8,000 B flash, 900 B RAM (driver 4,500 / 300, engine 3,500 / 600) |
| Driver route | Register driver, one chip (DW3110), one channel at compile time, no function table, no thread |
| Engine route | Pre-POLL, POLL, RESPONSE, FINAL_DATA as one state machine, STS key ladder on tinypsa AES-128-ECB, fixed-point DS-TWR, diagnostics off |
| Oracle | SPI register traces and walk-up recordings captured from the vendor build on the board (M0) |

## 1. Baseline: where the UWB bytes go

ultrawidelock `dwm3001cdk-lock`, `RELEASE=1 SMP=1`, NCS v3.3.0, LTO, 2026-10-18.
Symbols attributed to their defining file from DWARF; inlined callees count
toward their caller.

| File | Flash (B) | RAM (B) | Note |
|---|---:|---:|---|
| `dwt_uwb_driver/dw3000/dw3000_device.c` (9,521 lines, Qorvo) | 20,001 | 111 | `dwt_ioctl` dispatcher 6,768 B, 163 cases; PLL cal for ch5 and ch9 both linked 1,224 B; OTP programming 360 B; CIR and diagnostics readers about 1,500 B; TX power arithmetic 760 B |
| `src/deca_compat.c`, `deca_interface.c`, `deca_rsl.c`, `deca_port.c`, `qmath.c` | 1,460 | 184 | Function table (`dwt_probe`) binding the API to the chip. The table keeps every entry reachable, so LTO drops nothing |
| `ports/zephyr/dw3000/dw3000_hw.c`, `dw3000_spi.c` | 678 | 2,415 | IRQ GPIO, dedicated ISR workqueue thread with a 2,112 B stack, SPI transceive |
| **driver side** | **22,139** | **2,710** | |
| `src/ccc/ccc_shim_rx.c` (2,244 lines) | 7,124 | 764 | Pre-POLL decode 1,056 B, PHY apply 1,128 B, FINAL_DATA decode 1,396 B, RX re-arm 2,656 B; experiment flags, DIAGK trace, flight recorder hooks and range-bias calibration all inline |
| `src/ccc/ccc_kdf.c`, `ccc_shim.c`, `cherry_ccc_shim.c`, `ccc_mac.c`, `ccc_crypto_prim.c` | 2,419 | 128 | Key ladder (mUPSK, UAD, SP0 decrypt 544 B), session bind, MAC header parse |
| `src/driver/uwb_rxdiag.c` | 1,346 | 906 | Hard-coded under `CONFIG_ULTRAWIDELOCK_CRED`; ships in release |
| `src/cred/ultrawidelock_uwb_msg*.c`, `_session.c`, `_adapter.c`, `facade/` | 1,956 | 73 | M1-M4 codec, per-session state, facade |
| `src/driver/uwb_min.c`, `uwb_isr.c`, `fira/fira_session.c` | 110 | 79 | Bring-up, IRQ mask, URSK store |
| **engine side** | **12,955** | **1,501** | |
| **UWB total** | **35,094** | **4,211** | |

Also outside the bucket: about 2,500 B of soft-float helpers (`__mulsf3`,
`__addsf3`, `__divsf3`, compares) that nothing in tinylock links today. The
engine must stay integer.

## 2. The contract: what the engine asks of the chip

Taken from the call sites in ultrawidelock's own ISC files
(`modules/ultrawidelock_uwb/src/driver/uwb_min.c`, `uwb_isr.c`,
`src/ccc/ccc_shim_rx.c`, `ccc_shim_wrap.c`), not from the Qorvo headers. The
names below are tinyuwb's; the vendor names are listed once so M0 traces can
be matched, and are not an API tinyuwb keeps.

| Group | Operation | Vendor call it replaces | Used by |
|---|---|---|---|
| Bring-up | hardware reset pulse, wake via WAKEUP pin or CS, wait for IDLE_RC | `dwt_softreset`, `dwt_wakeup_ic`, `dwt_checkidlerc` | init |
| | read DEV_ID, expect `0xDECA0302` (DW3110) | `dwt_readdevid` | init, self-check |
| | initialise: OTP-sourced trims (LDO, bias, XTAL), PLL lock, receiver calibration | `dwt_initialise`, `dwt_setxtaltrim` | init, after wake |
| | configure PHY: channel, preamble length and code, PAC, SFD, data rate, PHR, SFD timeout, STS mode and length | `dwt_configure` | init, per ranging config |
| | configure TX RF: PG delay, power, PG count | `dwt_configuretxrf` | init |
| | antenna delays (TX, RX) | `dwt_settxantennadelay`, `dwt_setrxantennadelay` | init |
| Sleep (optional) | configure sleep and wake sources, enter sleep, restore after wake | `dwt_configuresleep`, `dwt_entersleep`, `dwt_restoreconfig` | `CONFIG_ULTRAWIDELOCK_UWB_DEEPSLEEP` only |
| Interrupts | enable mask; read and clear SYS_STATUS low word | `dwt_setinterrupt`, `dwt_readsysstatuslo`, `dwt_writesysstatuslo` | ISR |
| STS | set 128-bit key, set 128-bit IV, load IV (reset counter), set mode (off = SP0, no-data = SP3), read STS quality index, read STS status | `dwt_configurestskey`, `dwt_configurestsiv`, `dwt_configurestsloadiv`, `dwt_configurestsmode`, `dwt_readstsquality`, `dwt_readstsstatus` | every slot |
| TX | write TX data at offset, write frame control (length, offset, ranging bit), start immediate or delayed, read 40-bit TX timestamp | `dwt_writetxdata`, `dwt_writetxfctrl`, `dwt_starttx`, `dwt_readtxtimestamp` | RESPONSE |
| RX | enable immediate or delayed, set frame-wait timeout, force TRX off, read RX data, frame length, 40-bit Ipatov RX timestamp | `dwt_rxenable`, `dwt_setrxtimeout`, `dwt_forcetrxoff`, `dwt_readrxdata`, `dwt_getframelength`, `dwt_readrxtimestamp_ipatov` | Pre-POLL, POLL, FINAL_DATA |
| Time | read system time high 32 bits, set delayed TRX time (high 32 bits) | `dwt_readsystimestamphi32`, `dwt_setdelayedtrxtime` | slot scheduling |
| Diagnostics (off) | RX diagnostics, CIR, accumulator | `dwt_readdiagnostics`, `dwt_readcir`, `dwt_readctrdbg` | `TINYUWB_DIAG` only |
| Not carried | OTP programming, LEDs, DW3720 support, SCP preamble codes, PDoA, TDoA, Wi-Fi coexistence, repeated frames, timers, scratch RAM, SPI metrics | `dwt_otpprogword32`, `dwt_setleds`, `dwt_readpdoa`, `dwt_readtdoa`, `dwt_wifi_coex_set`, `dwt_stop_repeated_frames`, `dwt_timer_*`, `dwt_write_scratch_data` | nothing on the lock path |

About 35 operations. The vendor API linked today exposes about 171.

Interrupt mask the ISR unmasks (`uwb_isr.c:76-85`): RXFCG, RXFTO, RXPTO,
RXPHE, RXFCE, RXFSL, RXSTO, ARFE, CIAERR, TXFRS. High word zero.

PHY baseline (`uwb_min.c:114-140`): channel 9 (the responder reconfigures per
the ranging config), preamble length 128, PAC 8, preamble code 11, IEEE
802.15.4z 8-symbol SFD, 6.8 Mbps, standard PHR, SFD timeout 129, PG delay
`0x34`, power `0xfdfdfdfd`, PG count 0.

Slot timing the engine uses (`ccc_shim_rx.c:736-747, 1282-1311, 1553`), in
units of the system time high word (1 unit = 256 device time units, 1 DTU is
1 / (499.2 MHz x 128) s):

| Constant | Value | Meaning |
|---|---:|---|
| `CCC_SLOT_NOMINAL_HI32` | 499,200 | one slot, 127,795,200 DTU, 2.000 ms |
| `CCC_RESP_ANT_DLY_HI32` | 62 | responder antenna delay |
| `CCC_RX_SLOT_HI32` | 499,000 | RX arm point for the next slot |
| `CCC_RX_POLL_LEAD` | 40,000 | arm RX this much before the POLL |
| `CCC_RX_POLL_WIN_TO` | 1,350 | POLL frame-wait timeout |
| `CCC_FDRX_WIN_TO` | 7,800 | FINAL_DATA frame-wait timeout |
| `CCC_RESP_TXFRS_SPIN` | 3,000 | spin bound waiting for TXFRS |

## 3. Board wiring (DWM3001CDK)

From `apps/dwm3001cdk-lock/boards/decawave_dwm3001cdk.overlay` (ISC) and the
upstream Zephyr board files, which agree:

| Signal | Pin |
|---|---|
| SPI3 SCK | P0.03 |
| SPI3 MOSI | P0.08 |
| SPI3 MISO | P0.29 |
| CS | P1.06 (board-provided `&spi3` cs-gpios) |
| IRQ | P1.02, active high |
| RESETn | P0.25, active low |
| WAKEUP | P1.19, active high |

SPI 8 MHz for the first bring-up; raise only with a DEV_ID read loop as
evidence. The DW3110 inside the DWM3001C answers DEV_ID `0xDECA0302`.

## 4. What stays out, and why

| Today | tinyuwb | Saving (estimate) |
|---|---|---:|
| `dwt_ioctl` dispatcher and the probe function table | direct functions; the linker and LTO drop what the engine does not call | 7,000 B |
| PLL calibration for both channels, DW3720 compat, OTP programming, LEDs | one chip, `TINYUWB_CHANNEL` 5 or 9 at compile time | 3,000 B |
| CIR, accumulator and RX diagnostics, `uwb_rxdiag.c`, `g_cad` | `TINYUWB_DIAG` off by default | 2,800 B flash, 900 B RAM |
| Dedicated ISR workqueue thread | GPIO ISR reads SYS_STATUS, one `k_work` on the system workqueue; delayed-TX deadlines met by arming from the ISR path | 2,112 B RAM |
| `ccc_shim_rx.c` with experiments, DIAGK, flight recorder, range bias | one slot state machine, no hooks | 4,500 B |
| float in DS-TWR and the approach filter | Q16.16 fixed point | 2,500 B of soft-float, if nothing else keeps it |

## 5. References

- DW3000 Family User Manual 1.1
- DW3000 Data Sheet 1.3
- DW3000 API guide
- IEEE 802.15.4z

Choices the manual leaves open, marked REVIEW in `src/dw3000.c`:

| Step | Choice in `src/dw3000.c` | Why it is a choice |
|---|---|---|
| OTP word read | OTP_ADDR, then OTP_CFG = OTP_MAN with OTP_READ, read OTP_RDATA, OTP_CFG = 0 | 7.3.3 says "see the API" |
| LDO, bias and DGC kicks | the kick bit held 10 us, then OTP_CFG cleared | 8.2.12.3 does not say whether kicks self-clear |
| XTAL trim width | OTP bits 6:0 written whole | 8.2.10.4 shows 6 bits, table 17 stores 7 |
| XTAL trim fallback | 0x20 when the OTP word is 0 | no default given; the DWM3001C is calibrated, so unused |
| PLL_CAL value | 0x0081, then CAL_EN as 0x0181 | 8.2.10.3 gives the field value, not the sequence |
| RX calibration | COMP_DLY 2, CAL_MODE 1, CAL_EN, poll RX_CAL_STS, read both results, CAL_MODE 0 | 8.2.5.2 names the fields; the API guide's `dwt_pgf_cal(ldoen)` hints at LDO enables (LDO_CTRL, 8.2.8.9) it does not describe |
| ADC calibration | none | the API guide's DWT_ERR_RX_ADC_CAL has no counterpart in manual 1.1 |
| PLL_LOCK_CODE | not used; the default coarse code calibrates | OTP word layout against PLL_CC is not given |
| OTP antenna delays | not used; Kconfig values | the CDK manual names words 0x1A and 0x1C but not their layout |
| SYS_TIME read | a one-octet write, then the read | 8.2.2.7: a write releases the latch |
| DEEPSLEEP wake | RSTn pulse and a full rebuild | the AON restore path and its LDO reload are not needed for a lock |
| Receiver re-arm after errors | none in the driver; the engine arms | RXAUTR off, the manual's default |

## 6. Plan

Components (all estimates):

| Component | Scope | Flash (B) | RAM (B) |
|---|---|---:|---:|
| spi | 1- and 2-octet headers, read, write, masked write, burst to TX buffer and from RX buffer; Zephyr `spi_transceive` port | 350 | 0 |
| reg | the register subset of item 2: init with OTP trims and calibration, PHY configure for one channel, TX RF, antenna delay | 1,900 | 60 |
| trx | immediate and delayed TX and RX, timeouts, force off, timestamps, STS key, IV, load, mode | 1,100 | 40 |
| isr | GPIO interrupt, SYS_STATUS read and clear, event dispatch to one `k_work` | 300 | 100 |
| sleep | optional, behind `TINYUWB_SLEEP` | 400 | 16 |
| diag | optional, behind `TINYUWB_DIAG` | 0 | 0 |
| **driver** | | **4,050** | **216** |
| kdf | mUPSK, UAD, STS-V ladder on tinypsa AES-128-ECB; SP0 decrypt | 900 | 64 |
| round | Pre-POLL decode and session bind, POLL RX with CCC STS, RESPONSE delayed TX, FINAL_DATA decode, slot counter | 1,800 | 420 |
| twr | DS-TWR time of flight in Q16.16, range plausibility and STS quality gate, trust level | 500 | 48 |
| session | URSK, session id, last range, range listener | 250 | 60 |
| **engine** | | **3,450** | **592** |
| **tinyuwb** | | **7,500** | **808** |
| Target | | 8,000 | 900 |

Margin: 500 B flash, 92 B RAM. Thin; measured as each component lands, and
the item 2 list is cut, not grown, when a component overruns.

Host test strategy:

1. Host tests first, portable C11 under sanitizers: a fake SPI device records
   every transaction and answers reads from a register image.
2. Known-answer tests from the M0 traces: replay init, configure, one RX arm
   and one delayed TX, and compare tinyuwb's register writes with the
   vendor's. A difference is either a documented equivalence (noted beside
   the test) or a bug.
3. Engine known-answer tests from the M0 walk-up recordings: feed the
   recorded Pre-POLL, POLL and FINAL_DATA frames and timestamps through the
   round state machine; ranges must match the vendor engine's within DS-TWR
   rounding, and the STS-V per slot must be byte-identical.
4. Negative tests: short frames, bad MAC header, late delayed TX, RX timeout
   in each state, STS quality below threshold, slot counter wrap.
5. Size gate: `make size` on the Cortex-M4 cross build against the budget
   above, like tinyble's.

Milestones and gates:

| Milestone | Work | Go / no-go |
|---|---|---|
| M0 | Flash ultrawidelock with `CONFIG_DW3000_SPI_TRACE=y` and the flight recorder; capture init, configure, one full ranging round and one walk-up. Store under `tests/oracle/` as data, not code | Go when the register trace covers every operation in item 2 and the walk-up has at least one trusted range |
| M1 | spi, reg, trx, isr against the fake SPI device; replay of the M0 register trace | Go when every replay matches or has a noted equivalence, and the driver builds within 4,500 B flash, 300 B RAM |
| M2 | Bring-up on the board: DEV_ID, one TX, RX with a static STS from a second DWM3001CDK running ultrawidelock's `uwb_min_twr_poll` as the initiator | No-go if the receiver calibration or PLL lock cannot be reproduced from the manual plus the M0 trace; stop and list the missing registers |
| M3 | Engine on the board under MPSL timeslots with tinyble and tinygrid's radio running; replay of M0 walk-up frames on the host | Go when host replay matches and the board reports ranges from the initiator with no missed slot in 100 rounds |
| M4 | tinylock `stacks/tinyuwb.conf` with tinycred: Apple Home key unlock at the 1.00 m bound | Ship when 10 of 10 walk-ups unlock and the image stays within the size row the README gets |

M2 needs a second board or the DWM3000EVB as the initiator. If only one board
exists, M2 reduces to DEV_ID, TX with TXFRS, and RX timeout behaviour, and
the first real ranging is M4 against an iPhone.

## Reproduce

Sizes in item 1: build ultrawidelock with `make build PRISTINE=1 RELEASE=1
SMP=1` and attribute `build/<dir>/dwm3001cdk-lock/zephyr/zephyr.elf` by
defining file (DWARF line table for code, variable origin for data, `.a`
membership for binary libraries). The attribution script used on 2026-10-18
is not in either repository yet.

## 7. Phase 1 corrections (2026-10-19)

The engine side of ultrawidelock is the author's own code, so it was carried
over rather than rewritten: the CCC key ladder and SP0 CCM* (`kdf.c`), the
MAC codec and ranging schedule (`mac.c`), DS-TWR (`twr.c`), the STS keying
(`sts.c`), the session mapping (`session.c`), the range store with its
integrity layers (`range.c`), the ranging round (`round.c`, the single-core
DWM3001CDK path of `ccc_shim_rx.c` with the diagnostics, experiments and
other-platform branches removed) and the BLE Ranging Service M1-M4 setup
(`setup.c`, replacing the adapter, session, message codec and cherry shim
layers with one static single-session module). The radio is behind
`tinyuwb/hw.h`, the contract of item 2 reduced to 15 calls; the host tests
drive it through `tests/host/hw_fake.c`.

Host suite on 2026-10-19: 614 checks, 0 failures, under ASan and UBSan,
including the known-answer goldens that ultrawidelock's suite pinned and a
full DS-TWR round from genuine CCM*-encrypted Pre-POLL and Final_Data frames
to a latched 234 cm range.

Measured, Cortex-M4 `-Os`, each file alone, no LTO, no section garbage
collection (`make size`):

| File | Flash (B) | RAM (B) |
|---|---:|---:|
| kdf.c | 2,110 | 0 |
| mac.c | 1,024 | 0 |
| twr.c | 60 | 0 |
| sts.c | 660 | 128 |
| session.c | 98 | 0 |
| range.c | 684 | 73 |
| round.c | 2,134 | 476 |
| setup.c | 2,369 | 136 |
| **engine, one translation unit** | **9,229** | **813** |

Item 6 estimated 3,450 B for the engine. That number was wrong by a factor
of 2.7: it left out the M1-M4 setup service (2,369 B), the SP0 CCM* and
CMAC (most of kdf.c), and the Final_Data and Pre-POLL decoders. The
comparison that matters is the LTO image: ultrawidelock's engine side
measured 12,955 B there, carrying the same functions plus 4,500 B of
diagnostics and the adapter layers; the LTO build of tinylock with this
engine is the number to record next. The Makefile's gate is the measured
ceiling, 9,500 B and 900 B, until then.

Lines: tinyuwb 4,168 (sources and headers) against 8,963 in the ultrawidelock
files it replaces, plus 2,403 of host tests.

Not carried over, by decision: the satellite anchor handoff (two-board
side-of-door), the flight recorder, CIR and RX diagnostics, the lock-sweep
and pin probes, the ESP32 and nRF5340 branches, dynamic allocation.

## 8. Phase 2: the driver (2026-10-19)

`src/dw3000.c` implements `include/tinyuwb/hw.h` on `include/tinyuwb/bus.h`
(two SPI calls, reset, delay, IRQ enable), 15 functions and the ISR entry
`tu_dw3000_irq()`. It is portable C11: `tests/drv/bus_fake.c` is a DW3000 on
the host that decodes every header (one-octet, two-octet, masked write, fast
command) into a register image and answers RCINIT, SPIRDY, CPLOCK, the RX
calibration done flag, OTP reads, IV loads and HPDWARN; `tests/drv/test_dw3000.c`
pins the bring-up order, the channel tables, the refusals (-ENODEV, -EIO,
-ETIME, -EINVAL), the STS image byte order, the ISR flag mapping and the sleep
and rebuild path: 134 checks, ASan and UBSan clean.

What the driver does, by manual section:

| Step | Registers | Section |
|---|---|---|
| Reset, DEV_ID, wait RCINIT and SPIRDY | DEV_ID, SYS_STATUS | 2.4, 2.5, 8.2.2.1, 8.2.2.14 |
| OTP trims: LDO, bias kicks, XTAL_TRIM | OTP_ADDR, OTP_CFG, OTP_RDATA, XTAL | 7.3, 8.2.12, 8.2.10.4 |
| Channel: codes, SFD 4a, RF TX, PLL config, TX power | CHAN_CTRL, RF_TX_CTRL_1/2, LDO_RLOAD, PLL_CFG, TX_POWER | 8.2.2.22, 8.2.8, table 35, 8.2.2.21 |
| Receiver gain: OTP DGC kick or table 24, THR_64 | OTP_CFG, DGC_CFG0/1, DGC_LUT_0..6, DGC_CFG | 8.2.4, 8.2.12.3 |
| Digital RX: PAC 8, SFD timeout 65, no preamble timeout, DTUNE3 | DTUNE0, RX_SFD_TOC, PRE_TOC, DTUNE3 | 8.2.7 |
| SYS_CFG: std PHR, SP0, single buffer, both CIA passes | SYS_CFG | 8.2.2.4 |
| STS length 64, CIA STS tuning, antenna delays | STS_CFG, STS_CONF_1, TX_ANTD, CIA_CONF | 8.2.3.1, 8.2.13.40, 8.2.2.19, 8.2.13.36 |
| PLL calibrate, AINIT2IDLE, wait CPLOCK | PLL_CAL, SEQ_CTRL, SYS_STATUS | 8.2.10.3, 8.2.15.3 |
| RX calibration | RX_CAL, RX_CAL_STS, RX_CAL_RESI/Q | 8.2.5 |
| Interrupt mask, clear all | SYS_ENABLE, CMD_CLR_IRQS | 8.2.2.13, 9.19 |
| RX now or delayed, frame-wait timeout, late check | RX_FWTO, SYS_CFG.RXWTOE, DX_TIME, CMD_RX, CMD_DRX, HPDWARN | 4.3, 8.2.2.9, 8.2.2.11 |
| TX delayed with the ranging bit | TX_BUFFER, TX_FCTRL, DX_TIME, CMD_DTX, HPDWARN | 3.3, 8.2.2.8 |
| STS key, IV and latch, SP0 against SP3 | STS_KEY, STS_IV, STS_CTRL.LOAD_IV, SYS_CFG.CP_SPC | 6, 8.2.3 |
| STS quality against 60 % of the length | STS_STS | 6, 8.2.3.3 |
| Timestamps | IP_TS, TX_TIME, SYS_TIME | 8.2.13.1, 8.2.2.17, 8.2.2.7 |
| ISR: TXFRS, RXFCG or RXFR in SP3, timeouts, errors | SYS_STATUS, RX_FINFO | 8.2.2.14, 8.2.2.15 |
| DEEPSLEEP | AON_CFG, AON_CTRL | 8.2.11 |

Size, Cortex-M4 `-Os`, no LTO: 2,220 B flash, 28 B RAM against the item 6
budget of 4,500 and 300. The Zephyr port (`port/zephyr/bus_zephyr.c`,
`port/zephyr/port_zephyr.c`) adds the SPI and GPIO glue and tinypsa's AES.

Not in the driver, on purpose: double buffering, frame filtering, auto-ack,
sniff, preamble timeout, PDoA, CIR and diagnostics reads, OTP programming,
LEDs, the second SPI, SPI CRC, the AON restore path.
