# Bare-metal RV64 build of doomgeneric.
#
#   make                 build build/<platform>/headless/doom-riscv64.elf (timedemo benchmark)
#   make run             run it on QEMU virt and print the timedemo result
#   make run-ascii       timedemo at full speed, every 35th frame as ASCII text
#   make run-color       real-time demo playback, every frame in 24-bit colour,
#                        sized to fill the terminal (run-color-256 for 256 colours)
#   make run-play        play it yourself with the keyboard, 24-bit colour
#                        (run-play-256: 256 colours; run-play-ascii: ASCII over the UART only)
#   make play            build build/<platform>/play/: keyboard + ASCII over the UART, for hardware
#   make deterministic   build build/<platform>/deterministic/doom-riscv64.elf: same instruction
#                        stream on every core/simulator (virtual clock), for CPU studies
#   make newlib          (re)build the medany newlib this build links against
#   make wad             download the shareware doom1.wad (not kept in git) and check its SHA-1
#
# Boards: PLATFORM=qemu-virt (default, 16550 UART) or PLATFORM=qemu-sifive-u
# (SiFive UART, DOOM on hart 1), e.g. "make PLATFORM=qemu-sifive-u run".
# Everything target-specific is in platform/platform_config.h; override with
#   make PLATFORM_DEFINES="-DPLATFORM_UART_BASE_ADDRESS=0x..."

CROSS_COMPILE ?= riscv64-unknown-elf-
CC      := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
OBJDUMP := $(CROSS_COMPILE)objdump
SIZE    := $(CROSS_COMPILE)size
QEMU    ?= qemu-system-riscv64

PROJECT_DIRECTORY  := $(abspath .)
DOOM_SOURCE_DIRECTORY := $(PROJECT_DIRECTORY)/third_party/doomgeneric/doomgeneric
PLATFORM_DIRECTORY := $(PROJECT_DIRECTORY)/platform
BUILD_DIRECTORY    ?= $(PROJECT_DIRECTORY)/build
WAD_FILE           ?= $(PROJECT_DIRECTORY)/wad/doom1.wad

# The shareware WAD is id Software's copyright and may only be redistributed
# as the complete shareware package, so it is not kept in the repository.
# "make wad" fetches it; the SHA-1 check makes the source mirror irrelevant
# as long as the bytes match (override SHAREWARE_WAD_URL to use another one).
SHAREWARE_WAD_URL  ?= https://github.com/Akbar30Bill/DOOM_wads/raw/master/doom1.wad
SHAREWARE_WAD_SHA1 := 5b2e249b9c5133ec987b3ea77596381dc0d6bc1d
DEFAULT_WAD_FILE   := $(PROJECT_DIRECTORY)/wad/doom1.wad

# doomgeneric is a git submodule; a plain "git clone" leaves it empty and the
# compiler would only report a missing header.
TARGETS_WITHOUT_DOOM_SOURCE := wad clean distclean
ifeq ($(wildcard $(DOOM_SOURCE_DIRECTORY)/doomgeneric.h),)
ifneq ($(filter-out $(TARGETS_WITHOUT_DOOM_SOURCE),$(or $(MAKECMDGOALS),all)),)
$(error doomgeneric source is missing: run "git submodule update --init")
endif
endif

# The toolchain's prebuilt newlib is compiled with -mcmodel=medlow, which
# cannot address RAM at 0x8000_0000. A medany copy is built from the same
# newlib source tree into the build directory (see the "newlib" target).
NEWLIB_SOURCE_DIRECTORY  ?= $(HOME)/tools/riscv-gnu-toolchain/newlib
NEWLIB_INSTALL_DIRECTORY := $(BUILD_DIRECTORY)/newlib-medany
NEWLIB_TARGET_DIRECTORY  := $(NEWLIB_INSTALL_DIRECTORY)/riscv64-unknown-elf
NEWLIB_LIBRARY           := $(NEWLIB_TARGET_DIRECTORY)/lib/libc.a

ARCHITECTURE_FLAGS := -march=rv64gc -mabi=lp64d -mcmodel=medany

# Build-time behaviour, see platform/doomgeneric_baremetal.c.
DOOM_COMMAND_LINE          ?= doom -iwad doom1.wad -timedemo demo1 -nogui
FRAME_OUTPUT_INTERVAL          ?= 0
FRAME_RENDER_MODE              ?= ascii
FRAME_COLUMN_COUNT             ?= 0
SHOULD_REDRAW_IN_PLACE         ?= 0
SHOULD_USE_DETERMINISTIC_CLOCK ?= 0
# 1 = stdout goes through QEMU semihosting (fast; QEMU only), see platform_config.h.
SHOULD_USE_SEMIHOSTING_CONSOLE ?= 0
# 1 = read the keyboard from the UART (platform/keyboard_input.c).
SHOULD_READ_KEYBOARD           ?= 0
PLATFORM_DEFINES               ?=

# ---- Board profiles ----------------------------------------------------
# Each profile gives the platform_config.h overrides and the QEMU machine.
PLATFORM ?= qemu-virt

PROFILE_DEFINES_qemu-virt :=
QEMU_MACHINE_FLAGS_qemu-virt := -machine virt -cpu rv64 -smp 1 -m 128M
QEMU_NEEDS_SEMIHOSTING_qemu-virt := 0

# QEMU sifive_u (FU540 model), values read from its device tree:
#   UART0 "sifive,uart0" at 0x10010000, timebase-frequency 1 MHz,
#   hart 0 = rv64imac (no FPU), hart 1 = rv64imafdc, no "sifive,test" device.
# The UART clock (500 MHz = FU540 tlclk at 1 GHz core clock) only sets the
# baud divisor; QEMU ignores the divisor. There is no power-off device, so
# the program exits through semihosting.
SIFIVE_U_UART_INPUT_CLOCK_HZ := 500000000
PROFILE_DEFINES_qemu-sifive-u := \
    -DPLATFORM_UART_TYPE=PLATFORM_UART_TYPE_SIFIVE \
    -DPLATFORM_UART_BASE_ADDRESS=0x10010000 \
    -DPLATFORM_UART_INPUT_CLOCK_HZ=$(SIFIVE_U_UART_INPUT_CLOCK_HZ) \
    -DPLATFORM_MACHINE_TIMER_FREQUENCY_HZ=1000000 \
    -DPLATFORM_BOOT_HART_ID=1 \
    -DPLATFORM_EXIT_METHOD=PLATFORM_EXIT_METHOD_SEMIHOSTING
QEMU_MACHINE_FLAGS_qemu-sifive-u := -machine sifive_u -smp 2 -m 128M
QEMU_NEEDS_SEMIHOSTING_qemu-sifive-u := 1

ifeq ($(origin QEMU_MACHINE_FLAGS_$(PLATFORM)),undefined)
$(error PLATFORM must be qemu-virt or qemu-sifive-u, not "$(PLATFORM)")
endif

# A -DNAME=value in PLATFORM_DEFINES replaces the profile's -DNAME=..., so the
# compiler never sees the same macro defined twice.
define_name = $(firstword $(subst =, ,$(1)))
USER_DEFINE_NAMES := $(foreach definition,$(PLATFORM_DEFINES),$(call define_name,$(definition)))
KEPT_PROFILE_DEFINES := $(foreach definition,$(PROFILE_DEFINES_$(PLATFORM)),\
    $(if $(filter $(call define_name,$(definition)),$(USER_DEFINE_NAMES)),,$(definition)))
ALL_PLATFORM_DEFINES := $(strip $(KEPT_PROFILE_DEFINES) $(PLATFORM_DEFINES))

# FRAME_RENDER_MODE names -> the constants in platform/terminal_renderer.h.
RENDER_MODE_CONSTANT_ascii      := FRAME_RENDER_MODE_ASCII
RENDER_MODE_CONSTANT_truecolor  := FRAME_RENDER_MODE_TRUECOLOR
RENDER_MODE_CONSTANT_palette256 := FRAME_RENDER_MODE_PALETTE256
RENDER_MODE_CONSTANT := $(RENDER_MODE_CONSTANT_$(FRAME_RENDER_MODE))
ifeq ($(RENDER_MODE_CONSTANT),)
$(error FRAME_RENDER_MODE must be ascii, truecolor or palette256, not "$(FRAME_RENDER_MODE)")
endif

# DOOM's game clock runs at 35 tics per second, so printing every 35th frame
# during "make run-ascii" shows about one frame per second of demo time.
RUN_ASCII_FRAME_INTERVAL := 35

# -playdemo runs the demo at real speed (35 tics per second) and quits when it
# ends, unlike -timedemo which runs as fast as the CPU can.
REAL_TIME_DEMO_COMMAND_LINE := doom -iwad doom1.wad -playdemo demo1 -nogui

# No demo argument: DOOM starts at the title screen and waits for the player.
PLAY_COMMAND_LINE := doom -iwad doom1.wad -nogui
# The keyboard targets share these; a game has no natural end, so no timeout
# (GNU timeout treats 0 as "never").
PLAY_OPTIONS := SHOULD_READ_KEYBOARD=1 FRAME_OUTPUT_INTERVAL=1 SHOULD_REDRAW_IN_PLACE=1 \
    RUN_TIMEOUT_SECONDS=0 'DOOM_COMMAND_LINE=$(PLAY_COMMAND_LINE)'

# DOOM renders natively at 320x200; asking doomgeneric for exactly that size
# avoids an extra scaling pass on every frame.
FRAMEBUFFER_WIDTH  := 320
FRAMEBUFFER_HEIGHT := 200

# Upstream doomgeneric is old C: pin the dialect, since GCC 15 defaults to C23
# where "bool"/"true"/"false" are keywords and clash with DOOM's own enum.
COMMON_FLAGS := $(ARCHITECTURE_FLAGS) -std=gnu99 -O2 -g \
    -ffunction-sections -fdata-sections -fno-common \
    -isystem $(NEWLIB_TARGET_DIRECTORY)/include \
    -I$(PLATFORM_DIRECTORY) -I$(DOOM_SOURCE_DIRECTORY) \
    -DDOOMGENERIC_RESX=$(FRAMEBUFFER_WIDTH) -DDOOMGENERIC_RESY=$(FRAMEBUFFER_HEIGHT)

DOOM_FLAGS := $(COMMON_FLAGS) -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -w

# Upstream DOOM sources never include platform_config.h, so PLATFORM_DEFINES
# and the options below only affect the per-variant platform objects.
PLATFORM_FLAGS := $(COMMON_FLAGS) -Wall -Wextra $(ALL_PLATFORM_DEFINES) \
    '-DDOOM_COMMAND_LINE="$(DOOM_COMMAND_LINE)"' \
    -DFRAME_OUTPUT_INTERVAL=$(FRAME_OUTPUT_INTERVAL) \
    -DFRAME_RENDER_MODE=$(RENDER_MODE_CONSTANT) \
    -DFRAME_COLUMN_COUNT=$(FRAME_COLUMN_COUNT) \
    -DSHOULD_REDRAW_IN_PLACE=$(SHOULD_REDRAW_IN_PLACE) \
    -DPLATFORM_HAS_SEMIHOSTING_CONSOLE=$(SHOULD_USE_SEMIHOSTING_CONSOLE) \
    -DSHOULD_USE_DETERMINISTIC_CLOCK=$(SHOULD_USE_DETERMINISTIC_CLOCK) \
    -DSHOULD_READ_KEYBOARD=$(SHOULD_READ_KEYBOARD)

LINK_FLAGS := $(ARCHITECTURE_FLAGS) -nostartfiles -nostdlib -static \
    -Wl,--gc-sections -Wl,--wrap=I_Error \
    -Wl,--wrap=G_DeferedPlayDemo -Wl,--wrap=G_TimeDemo \
    -L$(NEWLIB_TARGET_DIRECTORY)/lib

# The same object list as upstream's Makefile, minus its X11 backend.
DOOM_OBJECT_NAMES := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad \
    d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus \
    i_endoom i_joystick i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat \
    m_config m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy \
    p_floor p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup \
    p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main \
    r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound \
    tables v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc \
    i_input i_video doomgeneric

DOOM_OBJECTS := $(addprefix $(BUILD_DIRECTORY)/doom/,$(addsuffix .o,$(DOOM_OBJECT_NAMES)))

# Objects that depend on the board and options get their own directory per
# configuration, so "make" and "make run-ascii" do not clobber each other.
VARIANT_NAME ?= headless
VARIANT_DIRECTORY := $(BUILD_DIRECTORY)/$(PLATFORM)/$(VARIANT_NAME)

PLATFORM_OBJECTS := \
    $(VARIANT_DIRECTORY)/start.o \
    $(VARIANT_DIRECTORY)/platform_hardware.o \
    $(VARIANT_DIRECTORY)/system_calls.o \
    $(VARIANT_DIRECTORY)/doomgeneric_baremetal.o \
    $(VARIANT_DIRECTORY)/compiler_runtime.o \
    $(VARIANT_DIRECTORY)/terminal_renderer.o \
    $(VARIANT_DIRECTORY)/keyboard_input.o \
    $(VARIANT_DIRECTORY)/wad_image.o

# Records the options this variant was last built with. Rewritten (and so
# made newer than the objects) only when an option changes, which forces the
# platform objects and linker script to rebuild instead of silently reusing
# ones built with different options.
CONFIGURATION_STAMP := $(VARIANT_DIRECTORY)/build_configuration.txt
CONFIGURATION_TEXT  := $(PLATFORM_FLAGS) WAD_FILE=$(WAD_FILE)
ifneq ($(file < $(CONFIGURATION_STAMP)),$(CONFIGURATION_TEXT))
$(shell mkdir -p $(VARIANT_DIRECTORY))
$(file > $(CONFIGURATION_STAMP),$(CONFIGURATION_TEXT))
endif

LINKER_SCRIPT := $(VARIANT_DIRECTORY)/linker_script.ld
OUTPUT_ELF    := $(VARIANT_DIRECTORY)/doom-riscv64.elf
OUTPUT_BINARY := $(VARIANT_DIRECTORY)/doom-riscv64.bin

QEMU_FLAGS := $(QEMU_MACHINE_FLAGS_$(PLATFORM)) -bios none -nographic -no-reboot
ifneq ($(filter 1,$(SHOULD_USE_SEMIHOSTING_CONSOLE) $(QEMU_NEEDS_SEMIHOSTING_$(PLATFORM))),)
QEMU_FLAGS += -semihosting-config enable=on,target=native
endif
RUN_TIMEOUT_SECONDS ?= 300

.PHONY: all run run-ascii run-color run-color-256 run-play run-play-256 run-play-ascii play deterministic run-deterministic newlib wad disassembly clean distclean

all: $(OUTPUT_ELF) $(OUTPUT_BINARY)

$(NEWLIB_LIBRARY):
	$(MAKE) newlib

wad: $(DEFAULT_WAD_FILE)

# Downloads to a temporary name first so an interrupted or wrong download
# never leaves a file that looks valid to the next make.
$(DEFAULT_WAD_FILE):
	@mkdir -p $(dir $@)
	curl -fL --retry 3 -o $@.download $(SHAREWARE_WAD_URL)
	@echo "$(SHAREWARE_WAD_SHA1)  $@.download" | sha1sum -c - \
	    || { rm -f $@.download; echo "doom1.wad checksum mismatch, expected shareware v1.9" >&2; exit 1; }
	@mv $@.download $@

newlib:
	mkdir -p $(BUILD_DIRECTORY)/newlib-build
	cd $(BUILD_DIRECTORY)/newlib-build && $(NEWLIB_SOURCE_DIRECTORY)/configure \
	    --target=riscv64-unknown-elf --prefix=$(NEWLIB_INSTALL_DIRECTORY) \
	    --disable-multilib --enable-newlib-io-long-double --enable-newlib-io-long-long \
	    --enable-newlib-io-c99-formats --enable-newlib-register-fini \
	    CFLAGS_FOR_TARGET="-O2 -D_POSIX_MODE -ffunction-sections -fdata-sections $(ARCHITECTURE_FLAGS)" \
	    > configure.log
	$(MAKE) -C $(BUILD_DIRECTORY)/newlib-build > $(BUILD_DIRECTORY)/newlib-build/make.log
	$(MAKE) -C $(BUILD_DIRECTORY)/newlib-build install > $(BUILD_DIRECTORY)/newlib-build/install.log

$(BUILD_DIRECTORY)/doom/%.o: $(DOOM_SOURCE_DIRECTORY)/%.c | $(NEWLIB_LIBRARY)
	@mkdir -p $(dir $@)
	@echo "  CC   $(notdir $<)"
	@$(CC) $(DOOM_FLAGS) -c $< -o $@

$(VARIANT_DIRECTORY)/%.o: $(PLATFORM_DIRECTORY)/%.c $(PLATFORM_DIRECTORY)/platform_config.h $(PLATFORM_DIRECTORY)/platform_hardware.h $(PLATFORM_DIRECTORY)/terminal_renderer.h $(PLATFORM_DIRECTORY)/keyboard_input.h $(CONFIGURATION_STAMP) | $(NEWLIB_LIBRARY)
	@mkdir -p $(dir $@)
	@echo "  CC   $(notdir $<)"
	@$(CC) $(PLATFORM_FLAGS) -c $< -o $@

$(VARIANT_DIRECTORY)/start.o: $(PLATFORM_DIRECTORY)/start.S $(PLATFORM_DIRECTORY)/platform_config.h $(CONFIGURATION_STAMP)
	@mkdir -p $(dir $@)
	@echo "  AS   $(notdir $<)"
	@$(CC) $(ARCHITECTURE_FLAGS) -I$(PLATFORM_DIRECTORY) $(ALL_PLATFORM_DEFINES) -c $< -o $@

$(VARIANT_DIRECTORY)/wad_image.o: $(PLATFORM_DIRECTORY)/wad_image.S $(WAD_FILE) $(CONFIGURATION_STAMP)
	@mkdir -p $(dir $@)
	@echo "  AS   $(notdir $<) <- $(notdir $(WAD_FILE))"
	@$(CC) $(ARCHITECTURE_FLAGS) '-DWAD_FILE_PATH="$(WAD_FILE)"' -c $< -o $@

$(LINKER_SCRIPT): $(PLATFORM_DIRECTORY)/linker_script.ld.in $(PLATFORM_DIRECTORY)/platform_config.h $(CONFIGURATION_STAMP)
	@mkdir -p $(dir $@)
	@$(CC) -E -P -x c -I$(PLATFORM_DIRECTORY) $(ALL_PLATFORM_DEFINES) $< -o $@

$(OUTPUT_ELF): $(PLATFORM_OBJECTS) $(DOOM_OBJECTS) $(LINKER_SCRIPT)
	@echo "  LD   $(notdir $@)"
	@$(CC) $(LINK_FLAGS) -T $(LINKER_SCRIPT) -Wl,-Map,$(VARIANT_DIRECTORY)/doom-riscv64.map \
	    $(PLATFORM_OBJECTS) $(DOOM_OBJECTS) \
	    -Wl,--start-group -lc -lm -lgcc -Wl,--end-group -o $@
	@$(SIZE) $@

$(OUTPUT_BINARY): $(OUTPUT_ELF)
	@$(OBJCOPY) -O binary $< $@

# --foreground matters: plain "timeout" moves QEMU into a background process
# group, and when QEMU then switches the terminal to raw mode (-nographic) the
# kernel stops it with SIGTTOU, so nothing is ever printed.
run: $(OUTPUT_ELF)
	timeout --foreground $(RUN_TIMEOUT_SECONDS) $(QEMU) $(QEMU_FLAGS) -kernel $<

run-ascii:
	$(MAKE) VARIANT_NAME=ascii FRAME_OUTPUT_INTERVAL=$(RUN_ASCII_FRAME_INTERVAL) run

run-color:
	$(MAKE) VARIANT_NAME=color FRAME_RENDER_MODE=truecolor FRAME_OUTPUT_INTERVAL=1 \
	    SHOULD_USE_SEMIHOSTING_CONSOLE=1 \
	    SHOULD_REDRAW_IN_PLACE=1 'DOOM_COMMAND_LINE=$(REAL_TIME_DEMO_COMMAND_LINE)' run

run-color-256:
	$(MAKE) VARIANT_NAME=color256 FRAME_RENDER_MODE=palette256 FRAME_OUTPUT_INTERVAL=1 \
	    SHOULD_USE_SEMIHOSTING_CONSOLE=1 \
	    SHOULD_REDRAW_IN_PLACE=1 'DOOM_COMMAND_LINE=$(REAL_TIME_DEMO_COMMAND_LINE)' run

# Keyboard play under QEMU: picture through semihosting (fast), keys through
# the UART. Quit with Ctrl-C (QEMU passes it to the game) or the DOOM menu.
run-play:
	$(MAKE) VARIANT_NAME=play-color FRAME_RENDER_MODE=truecolor SHOULD_USE_SEMIHOSTING_CONSOLE=1 \
	    $(PLAY_OPTIONS) run

run-play-256:
	$(MAKE) VARIANT_NAME=play-color256 FRAME_RENDER_MODE=palette256 SHOULD_USE_SEMIHOSTING_CONSOLE=1 \
	    $(PLAY_OPTIONS) run

# Everything over the UART, as on hardware without a debugger.
run-play-ascii:
	$(MAKE) VARIANT_NAME=play FRAME_RENDER_MODE=ascii $(PLAY_OPTIONS) run

play:
	$(MAKE) VARIANT_NAME=play FRAME_RENDER_MODE=ascii $(PLAY_OPTIONS) all

deterministic:
	$(MAKE) VARIANT_NAME=deterministic SHOULD_USE_DETERMINISTIC_CLOCK=1 all

run-deterministic:
	$(MAKE) VARIANT_NAME=deterministic SHOULD_USE_DETERMINISTIC_CLOCK=1 run

disassembly: $(OUTPUT_ELF)
	$(OBJDUMP) -d $< > $(VARIANT_DIRECTORY)/doom-riscv64.dis

# Keeps the medany newlib (slow to rebuild); "make distclean" removes it too.
clean:
	rm -rf $(BUILD_DIRECTORY)/doom $(BUILD_DIRECTORY)/qemu-virt $(BUILD_DIRECTORY)/qemu-sifive-u

distclean:
	rm -rf $(BUILD_DIRECTORY)
