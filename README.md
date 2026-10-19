# tinyuwb

A DW3000-family UWB driver and FiRa/CCC responder ranging engine
for tinylock on the nRF52833 (DWM3001CDK, DW3110 inside). It replaces the
vendored Qorvo driver and the UWB engine that ultrawidelock carries, as a
Zephyr module beside tinytalk, tinyble, tinygrid, tinypsa and tinyfota.
Portable C11. RESEARCH.md holds the Phase 0 research, the contract and the milestones.

## Status

Phase 2, 2026-10-19: the engine (key ladder, MAC codec, DS-TWR, STS keying, range store,
ranging round, M1-M4 setup service) is in `src/`, host-tested behind the radio contract in
`include/tinyuwb/hw.h`; the driver `src/dw3000.c` implements that contract on the bus seam `include/tinyuwb/bus.h`, host-tested against a fake
chip in `tests/drv/`. `port/zephyr/` carries the SPI, GPIO and AES glue. Not yet run on a
board.

## Size

Engine measured 2026-10-19 at 9,229 B flash and 813 B RAM as one translation unit at
`-Os` without LTO (RESEARCH.md item 7); the driver at 2,220 B and 28 B (item 8), against
35,094 B and 4,211 B for the vendor driver and engine in ultrawidelock's release image on
2026-10-18 (item 1). `make size` gates both.

## License

Apache-2.0, see LICENSE and NOTICE.
