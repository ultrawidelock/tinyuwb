# tinyuwb host build: unit tests, sanitizers, and the Cortex-M4 size gate.
#
# CC is honoured for the host. ARM_CC and ARM_SIZE pick the cross compiler;
# the nRF Connect SDK's arm-zephyr-eabi-gcc is what the firmware uses.

export LC_ALL := C
BUILD := build/host

CSTD := -std=c11 -O1
WARN := -Wall -Wextra -Wformat -Wformat-security -Wdouble-promotion -Wpointer-arith -Werror
SAN := -g -fsanitize=address,undefined -fno-sanitize-recover=all

INCS := -Iinclude
DRV := src/dw3000.c
ENGINE := $(filter-out $(DRV),$(wildcard src/*.c))
HDRS := $(wildcard include/tinyuwb/*.h) $(wildcard src/*.h)
HOST_PORT := $(wildcard port/host/*.c)
TESTS := $(wildcard tests/host/*.c)
DRV_TESTS := $(wildcard tests/drv/*.c) tests/host/test.c

ARM_CC ?= arm-none-eabi-gcc
ARM_SIZE ?= $(patsubst %gcc,%size,$(ARM_CC))
ARM_FLAGS := -std=c11 -Os -mcpu=cortex-m4 -mthumb -mabi=aapcs -ffunction-sections \
	-fdata-sections -fno-common $(WARN) $(INCS)
# Engine ceiling, measured 2026-10-19 (RESEARCH.md item 7): 9,229 B flash, 813 B RAM
# standalone at -Os. Driver ceiling from the plan in item 6.
SIZE_FLASH_MAX := 9500
SIZE_RAM_MAX := 900
DRV_FLASH_MAX := 4500
DRV_RAM_MAX := 300

.PHONY: test asan size clean

$(BUILD)/test: $(ENGINE) $(HOST_PORT) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(INCS) -Itests/host $(ENGINE) $(HOST_PORT) $(TESTS) -o $@

$(BUILD)/test-drv: $(DRV) $(DRV_TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(WARN) $(INCS) -Isrc -Itests/host $(DRV) $(DRV_TESTS) -o $@

$(BUILD)/asan: $(ENGINE) $(HOST_PORT) $(TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(SAN) $(WARN) $(INCS) -Itests/host $(ENGINE) $(HOST_PORT) $(TESTS) -o $@

$(BUILD)/asan-drv: $(DRV) $(DRV_TESTS) $(HDRS)
	@mkdir -p $(@D)
	$(CC) $(CSTD) $(SAN) $(WARN) $(INCS) -Isrc -Itests/host $(DRV) $(DRV_TESTS) -o $@

test: $(BUILD)/test $(BUILD)/test-drv
	$(BUILD)/test
	$(BUILD)/test-drv

asan: $(BUILD)/asan $(BUILD)/asan-drv
	$(BUILD)/asan
	$(BUILD)/asan-drv

# Each source alone, then the engine as one translation unit (what LTO sees), then the driver.
size:
	@mkdir -p $(BUILD)/size
	@for f in $(ENGINE) $(DRV); do \
		$(ARM_CC) $(ARM_FLAGS) -c $$f -o $(BUILD)/size/$$(basename $$f .c).o || exit 1; \
	done
	@$(ARM_SIZE) $(BUILD)/size/*.o
	@cat $(ENGINE) > $(BUILD)/size/all.c
	@$(ARM_CC) $(ARM_FLAGS) -c $(BUILD)/size/all.c -o $(BUILD)/size/all.o
	@$(ARM_SIZE) $(BUILD)/size/all.o | tail -1 | awk -v fm=$(SIZE_FLASH_MAX) -v rm=$(SIZE_RAM_MAX) \
		'{ flash = $$1 + $$2; ram = $$2 + $$3; \
		   printf "engine as one unit: %d B flash, %d B RAM (budget %d / %d)\n", flash, ram, fm, rm; \
		   if (flash > fm || ram > rm) { print "over budget"; exit 1 } }'
	@$(ARM_SIZE) $(BUILD)/size/dw3000.o | tail -1 | awk -v fm=$(DRV_FLASH_MAX) -v rm=$(DRV_RAM_MAX) \
		'{ flash = $$1 + $$2; ram = $$2 + $$3; \
		   printf "driver: %d B flash, %d B RAM (budget %d / %d)\n", flash, ram, fm, rm; \
		   if (flash > fm || ram > rm) { print "over budget"; exit 1 } }'

clean:
	rm -rf build
