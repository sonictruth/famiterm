# nesterm - FAM ROM built with LLVM-MOS SDK
NAME    := nesterm
BUILD   := build
SRC_DIR := romcart
CHR_DIR := $(SRC_DIR)/chr

# SDK lookup: $LLVM_MOS_HOME, then ./.sdk, then PATH
LLVM_MOS_HOME ?= $(CURDIR)/.sdk
ifneq ($(wildcard $(LLVM_MOS_HOME)/bin/mos-nes-mmc1-clang),)
  CC := $(LLVM_MOS_HOME)/bin/mos-nes-mmc1-clang
else
  CC := mos-nes-mmc1-clang
endif

CFLAGS  := -Os -flto -Wall -Wextra -Ifirmware -lneslib
# The link's cycle-timed UART, written for LINK_PERIOD by scripts/gen-link.py
LINK_PROTO := firmware/link-proto.h
LINK_ASM   := $(BUILD)/gen/link_rxtx.S
SRCS    := $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/*.S) $(LINK_ASM)
DEPS    := $(SRCS) $(wildcard $(SRC_DIR)/*.h) $(LINK_PROTO)
NM      := $(dir $(CC))llvm-nm
ROM     := $(BUILD)/$(NAME).nes
# Static font list: fonts/<name>.h -> romcart/chr/font_<name>.chr (order must match romcart/chr.S and screen.c)
FONT_NAMES := Altered_Chrome Around Bauhaus Computing_60s Security_Concepts ZX_Chicago ZX_OCR-A
FONT_CHRS  := $(FONT_NAMES:%=$(CHR_DIR)/font_%.chr)
SPLASH  := $(CHR_DIR)/splash.chr
SPLASH_H := $(CHR_DIR)/splash_pic.h
SPLASH_IMG := images/splash.png

.PHONY: all build clean run test check check-core bench nes-sdk sdk-esp32-s3 firmware firmware-upload romcart bridge shot
all: $(ROM)
# The NES ROM: build/nesterm.nes
romcart: $(ROM)
# ROM + ESP32-S3 firmware
build: romcart firmware

$(CHR_DIR):
	mkdir -p $@

$(CHR_DIR)/font_%.chr: fonts/%.h scripts/gen-fonts.py | $(CHR_DIR)
	python3 scripts/gen-fonts.py $@ --font $<

# one run produces both the CHR tiles and the nametable header
$(SPLASH): $(SPLASH_IMG) scripts/gen-splash.py | $(CHR_DIR)
	python3 scripts/gen-splash.py $(SPLASH_IMG) $(SPLASH) $(SPLASH_H)

# splash.chr's recipe creates the header too; regenerate only if it went missing
$(SPLASH_H): $(SPLASH)
	@test -f $@ || python3 scripts/gen-splash.py $(SPLASH_IMG) $(SPLASH) $@

$(LINK_ASM): $(LINK_PROTO) scripts/gen-link.py
	mkdir -p $(dir $@)
	python3 scripts/gen-link.py $(LINK_PROTO) $@

# Each timed routine must sit on one page: a branch across one costs a cycle
LINK_CHECK = python3 scripts/check-link-pages.py $(NM) $@.elf

$(ROM): $(DEPS) $(FONT_CHRS) $(SPLASH) $(SPLASH_H)
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRCS)
	$(LINK_CHECK)

nes-sdk:
	./scripts/setup-nes-sdk.sh

# --- ESP32-S3 firmware: fami-modem (firmware/) ---
# arduino-cli + the esp32:esp32 core live in ./.esp32-s3-sdk (installed by
# `make sdk-esp32-s3`; arduino-cli is the build tool, the ESP32-S3 support
# comes from the core)
ESP32_S3_CLI   ?= $(CURDIR)/.esp32-s3-sdk/bin/arduino-cli
ESP32_S3_DATA  ?= $(CURDIR)/.esp32-s3-sdk/data
ESP32_S3_BUILD := $(BUILD)/esp32-s3
# empty = auto-detect from the connected board; e.g. esp32:esp32:esp32s3:USBMode=HWDC
ESP32_S3_FQBN  ?=
# empty = first board found on a serial port
ESP32_S3_PORT  ?=
ESP32_S3_ENV   := ESP32_S3_CLI="$(ESP32_S3_CLI)" ESP32_S3_DATA="$(ESP32_S3_DATA)" \
                 ESP32_S3_BUILD="$(ESP32_S3_BUILD)" ESP32_S3_FQBN="$(ESP32_S3_FQBN)" \
                 ESP32_S3_PORT="$(ESP32_S3_PORT)"

sdk-esp32-s3:
	ARDUINO_DIRECTORIES_DATA="$(ESP32_S3_DATA)" ./scripts/setup-esp32-s3-sdk.sh

# Compile firmware/firmware.ino -> build/esp32-s3/firmware.ino.bin (board auto-detected)
firmware:
	@test -x "$(ESP32_S3_CLI)" || $(MAKE) --no-print-directory sdk-esp32-s3
	$(ESP32_S3_ENV) ./scripts/build-firmware.sh

# Build the ESP32-S3 firmware and flash it: make firmware-upload [ESP32_S3_PORT=/dev/cu.usbmodem14201]
firmware-upload:
	@test -x "$(ESP32_S3_CLI)" || $(MAKE) --no-print-directory sdk-esp32-s3
	$(ESP32_S3_ENV) ./scripts/build-firmware.sh --upload

# Mesen 2 (macOS app); override with MESEN_BIN=/path/to/Mesen
MESEN_BIN ?= /Applications/Mesen.app/Contents/MacOS/Mesen
# LUA: ESP32-S3 emulator script loaded alongside the ROM (make run LUA= to disable)
LUA ?= tools/faketty.lua
run: $(ROM)
	@test -x $(MESEN_BIN) || { echo "Mesen not found: get it from mesen.ca or set MESEN_BIN"; exit 1; }
	$(MESEN_BIN) --debug.scriptWindow.allowIoOsAccess=true $(ROM) $(LUA)

# Demo script (VT52 chart + numbered lines) in Mesen
test: $(ROM)
	$(MESEN_BIN) --debug.scriptWindow.allowIoOsAccess=true $(ROM) tools/test.lua

# The link end to end: tools/check.lua streams text through an emulated ESP32-S3
# (tools/esp-link.lua, a cycle-level UART) and compares the screen it leaves.
# Once on a clean line, then on one that is late, off-clock, lossy and noisy,
# and across a power cycle of the ESP32-S3.
MESEN_TEST = $(MESEN_BIN) --testRunner --debug.scriptWindow.allowIoOsAccess=true
check: check-core $(ROM)
	@test -x $(MESEN_BIN) || { echo "Mesen not found: get it from mesen.ca or set MESEN_BIN"; exit 1; }
	$(MESEN_TEST) $(ROM) tools/check.lua
	ESP_JITTER=3000 ESP_SKEW=0.02 $(MESEN_TEST) $(ROM) tools/check.lua
	ESP_JITTER=3000 ESP_SKEW=-0.02 CHECK_SEED=2 $(MESEN_TEST) $(ROM) tools/check.lua
	ESP_DROP=4 ESP_CORRUPT=5 CHECK_SEED=3 $(MESEN_TEST) $(ROM) tools/check.lua
	ESP_RESTART=420 CHECK_SEED=4 $(MESEN_TEST) $(ROM) tools/check.lua

# The firmware's link core (frames, CRC, services), on this computer
CXX ?= c++
CORE_TEST := $(BUILD)/link-core-test
check-core: $(CORE_TEST)
	$(CORE_TEST)
$(CORE_TEST): test/link-core-test.cpp firmware/link-core.cpp firmware/link-core.h $(LINK_PROTO)
	mkdir -p $(BUILD)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -g -fsanitize=address,undefined -Ifirmware \
	  -o $@ test/link-core-test.cpp firmware/link-core.cpp

# CPU and vblank budget with an endless stream: make bench [BENCH_PAT=lines]
SYMS := $(BUILD)/$(NAME).syms.lua
$(SYMS): $(ROM)
	{ echo "return {"; $(NM) $(ROM).elf | awk 'NF == 3 { printf "  [\"%s\"] = 0x%s,\n", $$3, $$1 }'; echo "}"; } > $@
bench: $(ROM) $(SYMS)
	BENCH_SYMS=$(abspath $(SYMS)) $(MESEN_TEST) $(ROM) tools/bench.lua

# Virtual serial port /tmp/ttyEMU bridged to the emulator (run `make run` first)
bridge:
	./scripts/serial-bridge.sh

# Headless run in the real Mesen (accurate); writes build/mesen.png
shot: $(ROM)
	SHOT_OUT=$(abspath $(BUILD))/mesen.png $(MESEN_BIN) --testRunner \
	  --debug.scriptWindow.allowIoOsAccess=true $(ROM) tools/mesen-shot.lua

clean:
	rm -rf $(BUILD) $(CHR_DIR)
