# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
cmake_minimum_required(VERSION 3.13)

# Board configuration variant (M1, M2, PC, Z2, C2 or NX)
set(BOARD "M2" CACHE STRING "Board variant: M1, M2, PC, Z2, C2 or NX")
# Alias for backwards compatibility
if(DEFINED BOARD_VARIANT AND NOT DEFINED BOARD)
    set(BOARD ${BOARD_VARIANT})
endif()

if(BOARD STREQUAL "Z2")
    # Waveshare RP2350-PiZero uses RP2350B (48 GPIOs). The stock pico2
    # board definition selects RP2350A and therefore caps GPIOs at 0..29,
    # breaking Z2 peripherals on GPIO30..47 and PIO's movable GPIO window.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "waveshare_rp2350_pizero" CACHE STRING "Pico board type" FORCE)
elseif(BOARD STREQUAL "C2")
    # FRANK Core 2 master is an RP2350B. The stock pico2 definition
    # declares PICO_RP2350A 1, capping NUM_BANK0_GPIOS at 30 and
    # switching off PIO's movable GPIO window — the link bus (GPIO20..42),
    # the WS2812 (GPIO46) and PSRAM CS (GPIO47) would alias down into
    # 0..29. Our own header sets PICO_RP2350A 0 / PICO_PIO_USE_GPIO_BASE 1.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "frank_core2_master" CACHE STRING "Pico board type" FORCE)
    # USB HID host is the only input path on C2 — no PS/2, no NES pad header.
    set(USB_HID_ENABLED ON CACHE BOOL "USB HID host" FORCE)
    # HDMI is the default video output; there is no analog VGA DAC to
    # auto-detect, so the pin probe is skipped either way. -DFORCE_VGA=ON
    # drives VGA timing out of GPIO12..19 instead, for bench debug.
    if(NOT FORCE_VGA)
        set(FORCE_HDMI ON CACHE BOOL "Force HDMI" FORCE)
    endif()
elseif(BOARD STREQUAL "NX")
    # FRANK Next master: RP2350B like C2's, with the console on GPIO46/45
    # (GPIO0/1 are the SDIO card's CMD and CLK). USB HID is the only input
    # and HDMI the only output, as on C2.
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards CACHE PATH "" FORCE)
    set(PICO_BOARD "frank_next_master" CACHE STRING "Pico board type" FORCE)
    # NX_USB_CDC: a bench build, the master's USB port the CDC console (and
    # ~BOOT~ for picotool) instead of the keyboard host. J2's RX does not
    # reach the RP on the bench board (GPIO45 stays high), so the UART
    # console is output only there.
    option(NX_USB_CDC "NX: USB CDC console instead of the USB HID host" OFF)
    if(NX_USB_CDC)
        set(USB_HID_ENABLED OFF CACHE BOOL "USB HID host" FORCE)
    else()
        set(USB_HID_ENABLED ON CACHE BOOL "USB HID host" FORCE)
    endif()
    if(NOT FORCE_VGA)
        set(FORCE_HDMI ON CACHE BOOL "Force HDMI" FORCE)
    endif()
else()
    # Set board to Pico 2 (RP2350)
    set(PICO_BOARD pico2 CACHE STRING "Pico board type")
endif()

set(BUILD_NAME "dos")

# Firmware version string (set by release.sh or manually)
# FIRMWARE_VERSION is derived from version.txt after parsing (see below)

# CPU overclock configuration (252, 378, 504 MHz)
# 252 - very basic overclock, 378 = stable overclock, 504 = high overclock
set(CPU_SPEED "504" CACHE STRING "CPU clock in MHz: 378, 504")

# PSRAM speed configuration (133, 166 MHz target)
set(PSRAM_SPEED "66" CACHE STRING "PSRAM max frequency in MHz: 66, 84, 100, 133, 166")
set(FLASH_SPEED "66" CACHE STRING "Flash (QSPI XIP) max frequency in MHz")

# Enable LTEMM support (performance degradation is expected)
option(EMM "Enable LTEMM support" OFF)

# USB HID keyboard support (disabled by default to keep USB CDC for debug)
option(USB_HID_ENABLED "Enable USB HID keyboard (disables USB CDC stdio)" ON)

# Z2 (RP2350-PiZero) USB host port: the board's "PIO-USB" USB-C socket
# (GPIO28/29, driven by Pico-PIO-USB) instead of the native "USB" one.
# TinyUSB runs one host controller, so it is one or the other.
option(Z2_PIO_USB "Z2: USB host on the PIO-USB port (OFF = native USB port)" ON)

# Force HDMI output (skip VGA/HDMI pin auto-detection)
option(FORCE_HDMI "Force HDMI output instead of VGA" OFF)

# Force VGA output (skip auto-detection). Takes precedence over FORCE_HDMI.
option(FORCE_VGA "Force VGA output instead of HDMI" OFF)

# Debug output (disabled by default for cleaner console)
option(DEBUG_ENABLED "Enable verbose debug output" OFF)

# Profiling (for performance analysis)
option(PROFILE_ENABLED "Enable i386 instruction profiling" OFF)

# Coarse per-subsystem cycle accounting on core 0 (docs/C2_SPLIT_PLAN.md).
# Distinct from PROFILE_ENABLED, which profiles instructions.
option(SUBSYS_PROFILE "Enable core-0 subsystem cycle profiling" OFF)

if(BOARD STREQUAL "M1")
    SET(BUILD_NAME "m1p2-${BUILD_NAME}")
elseif(BOARD STREQUAL "PC")
    SET(BUILD_NAME "PCp2-${BUILD_NAME}")
elseif(BOARD STREQUAL "Z2")
    SET(BUILD_NAME "z0p2-${BUILD_NAME}")
elseif(BOARD STREQUAL "C2")
    SET(BUILD_NAME "c2p2-${BUILD_NAME}")
elseif(BOARD STREQUAL "NX")
    SET(BUILD_NAME "nxp2-${BUILD_NAME}")
else()  # M2
    SET(BUILD_NAME "m2p2-${BUILD_NAME}")
endif()

# CPU voltage selection based on speed
if(CPU_SPEED GREATER_EQUAL 504)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_65")
elseif(CPU_SPEED GREATER_EQUAL 378)
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_60")
else()
    set(CPU_VOLTAGE "VREG_VOLTAGE_1_50")
endif()

message(STATUS "frank-dos: Board=${BOARD}, CPU=${CPU_SPEED}MHz, PSRAM=${PSRAM_SPEED}MHz, Voltage=${CPU_VOLTAGE}, Audio=runtime")

if(BOARD STREQUAL "Z2" AND USB_HID_ENABLED AND Z2_PIO_USB)
    set(PIO_USB_ENABLED ON)
    # Vendored Pico-PIO-USB 0.7.2 (sekigon-gonnoc, MIT). Must be set before
    # pico_sdk_init(): TinyUSB's rp2040 family script picks it up from here
    # and creates the tinyusb_pico_pio_usb target.
    set(PICO_PIO_USB_PATH ${CMAKE_CURRENT_LIST_DIR}/drivers/pio_usb)
else()
    set(PIO_USB_ENABLED OFF)
endif()

include(pico_sdk_import.cmake)

# Static libraries in the build directory: in the source tree's bin/ every
# build directory shared one libsdcard.a, and a C2 build's (SPI) was linked
# into the NX one (SDIO) that came after it.
set(OUTPUT_DIR "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")

project(${BUILD_NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)

# Read version from version.txt (format: "MAJOR MINOR" e.g. "1 00" = v1.00)
file(READ "${CMAKE_CURRENT_LIST_DIR}/version.txt" VERSION_CONTENT)
string(STRIP "${VERSION_CONTENT}" VERSION_CONTENT)
string(REPLACE " " ";" VERSION_LIST "${VERSION_CONTENT}")
list(GET VERSION_LIST 0 FRANK_DOS_VERSION_MAJOR)
list(GET VERSION_LIST 1 FRANK_DOS_VERSION_MINOR)
string(LENGTH "${FRANK_DOS_VERSION_MINOR}" _minor_len)
if(_minor_len LESS 2)
    set(FIRMWARE_VERSION "v${FRANK_DOS_VERSION_MAJOR}.0${FRANK_DOS_VERSION_MINOR}")
else()
    set(FIRMWARE_VERSION "v${FRANK_DOS_VERSION_MAJOR}.${FRANK_DOS_VERSION_MINOR}")
endif()
message(STATUS "frank-dos version: ${FIRMWARE_VERSION}")

pico_sdk_init()

#=============================================================================
# FatFS Library
#=============================================================================
add_library(fatfs
    drivers/fatfs/ff.c
    drivers/fatfs/ffsystem.c
    drivers/fatfs/ffunicode.c
    drivers/fatfs/f_util.c
)
target_include_directories(fatfs PUBLIC drivers/fatfs)
target_link_libraries(fatfs pico_stdlib)

#=============================================================================
# SD Card Driver
#=============================================================================
if(BOARD STREQUAL "NX")
# FRANK Next: the card in 4-bit SDIO (rabbitholecomputing's SDIO_RP2350, MIT,
# with frank-react's FatFs diskio). Same target name, so nothing else changes.
# A card left in SPI mode (frank-test checks cards in SPI mode; only a power
# cycle undoes it) falls back to drivers/sdcard's PIO SPI on the same pins,
# its FatFs entry points renamed spi_disk_* (sd_sdio.c).
add_library(sdcard
    drivers/sdio/sdio_rp2350.cpp
    drivers/sdio/sd_sdio.c
    drivers/sdcard/sdcard.c
    drivers/sdcard/pio_spi.c
)
target_include_directories(sdcard PUBLIC drivers/sdio drivers/sdcard drivers/fatfs src)
target_compile_definitions(sdcard PRIVATE BOARD_${BOARD} RP2350_BUILD=1 SD_SPI_FALLBACK=1)
# Single sectors read as their 8-sector group (sd_sdio.c). Reading an 11 MB
# file twice through the emulated IDE disk: 14.1 / 15.0 s without, 5.5 / 5.5 s
# with.
# The OPL2 synth on core 1 (adlib.c: adlib_core1_poll) instead of a 1 kHz
# timer on core 0, where it was ~18% of the emulation in Doom with music.
option(NX_ADLIB_CORE1 "NX: render AdLib music on core 1" ON)
option(NX_READAHEAD "NX: read single sectors as their 8-sector group" ON)
if(NOT NX_READAHEAD)
    target_compile_definitions(sdcard PRIVATE SD_READAHEAD=0)
endif()
if(NX_ADLIB_CORE1)
    set(ADLIB_CORE1_DEF ADLIB_CORE1=1)
endif()
# The slave as a cache of the card's groups (slave/sdcache, link/sc_master.c):
# frank-react's link session over link_bus, PIO2. Off by default: a group back
# from the slave cost what one from the card does (~0.9 ms: three control
# frames, 4 KB over the link and its PSRAM copy against one 8-sector SDIO
# command), and putting every group there made a cold read 30% slower
# (7.2 / 5.7 s on the test above).
option(NX_SLAVE_CACHE "NX: cache SD sectors in the slave's PSRAM" OFF)
if(NX_SLAVE_CACHE)
    target_sources(sdcard PRIVATE link/link_bus.c link/link_session.c link/link_proto.c link/sc_master.c)
    target_include_directories(sdcard PUBLIC link)
    target_compile_definitions(sdcard PUBLIC SD_SLAVE_CACHE=1 LINK_CTRL_CLKDIV=4.0f)
    pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/link/link_bus.pio)
    target_link_libraries(sdcard hardware_gpio)
endif()
set_source_files_properties(drivers/sdcard/sdcard.c PROPERTIES COMPILE_DEFINITIONS
    "SDCARD_PIO=1;disk_initialize=spi_disk_initialize;disk_status=spi_disk_status;disk_read=spi_disk_read;disk_write=spi_disk_write;disk_ioctl=spi_disk_ioctl;get_fattime=spi_get_fattime")
target_link_libraries(sdcard pico_stdlib hardware_pio hardware_dma hardware_clocks)
pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/drivers/sdio/sdio_rp2350.pio)
pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard/spi.pio)
else()
add_library(sdcard
    drivers/sdcard/sdcard.c
    drivers/sdcard/pio_spi.c
)
target_include_directories(sdcard PUBLIC drivers/sdcard drivers/fatfs src)
target_compile_definitions(sdcard PRIVATE
    BOARD_${BOARD}
    RP2350_BUILD=1
    # SDCARD_PIO=1  # Disabled - use hardware SPI instead
)
target_link_libraries(sdcard pico_stdlib hardware_spi hardware_pio hardware_dma)
pico_generate_pio_header(sdcard ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard/spi.pio)
endif()

#=============================================================================
# Unified PS/2 Driver (keyboard + mouse on shared PIO)
#=============================================================================
add_library(ps2_driver
    drivers/ps2/ps2.c
)
target_include_directories(ps2_driver PUBLIC drivers/ps2 src)
target_compile_definitions(ps2_driver PRIVATE
    BOARD_${BOARD}
)
target_link_libraries(ps2_driver pico_stdlib hardware_pio hardware_gpio hardware_clocks)
pico_generate_pio_header(ps2_driver ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2/ps2.pio)

#=============================================================================
# NES/SNES Gamepad Driver
#=============================================================================
add_library(nespad
    drivers/nespad/nespad.c
)
target_include_directories(nespad PUBLIC drivers/nespad src)
target_compile_definitions(nespad PRIVATE
    BOARD_${BOARD}
)
target_link_libraries(nespad pico_stdlib hardware_pio hardware_gpio)

#=============================================================================
# PSRAM Driver
#=============================================================================
add_library(psram
    drivers/psram/psram_init.c
)
target_include_directories(psram PUBLIC drivers/psram src)
target_compile_definitions(psram PRIVATE
    BOARD_${BOARD}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
)
target_link_libraries(psram pico_stdlib hardware_clocks)

#=============================================================================
# VGA Video Driver
#=============================================================================
add_library(video_driver
    drivers/hdmi/hdmi.c
    drivers/hdmi/hstx_packet.c
    drivers/vga/test_pins.c
    drivers/vga/vga_hw.c
    drivers/vga/vga_osd.c
    drivers/vga/font8x16.c
)
target_include_directories(video_driver PUBLIC drivers/vga drivers/hdmi src)
target_compile_definitions(video_driver PRIVATE
    BOARD_${BOARD}
    RP2350_BUILD=1
)
target_link_libraries(video_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync)
# the HDMI line IRQ must stay in SRAM: no loops turned into newlib calls
set_source_files_properties(drivers/hdmi/hdmi.c PROPERTIES COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")

#=============================================================================
# Runtime I2S/PWM Audio Driver
#=============================================================================
add_library(audio_driver
    drivers/audio/audio.c
    $<$<STREQUAL:${BOARD},NX>:${CMAKE_CURRENT_LIST_DIR}/drivers/audio/tlv320.c>
)
target_include_directories(audio_driver PUBLIC drivers/audio src)
target_compile_definitions(audio_driver PRIVATE
    BOARD_${BOARD}
    SOUND_FREQUENCY=44100
)
pico_generate_pio_header(audio_driver ${CMAKE_CURRENT_LIST_DIR}/drivers/audio/audio_i2s.pio)
target_link_libraries(audio_driver pico_stdlib hardware_dma hardware_pio hardware_clocks hardware_sync hardware_irq hardware_pwm hardware_i2c video_driver)

#=============================================================================
# USB HID Keyboard Driver (optional)
#=============================================================================
add_subdirectory(drivers/usbhid)

if(EMM)
    SET(BUILD_NAME "${BUILD_NAME}-${CPU_SPEED}MHz-P${PSRAM_SPEED}-${FIRMWARE_VERSION}-emm")
else ()
    SET(BUILD_NAME "${BUILD_NAME}-${CPU_SPEED}MHz-P${PSRAM_SPEED}-${FIRMWARE_VERSION}")
endif()

#=============================================================================
# Core 386 Emulator Sources
#=============================================================================
set(EMU_CORE_SOURCES
    # CPU and core
    src/i386.c       # frank-react's core: fastloop.inc is #included here
    src/pc.c

    # Peripheral chips
    src/i8042.c      # Keyboard controller
    src/i8254.c      # Timer
    src/i8257.c      # DMA
    src/i8259.c      # PIC

    # Devices
    src/vga.c        # VGA emulation
    src/disk.c       # INT 13h disk handler (from pico-286)
    src/ide.c
    src/fdd.c
    src/pci.c        # PCI bus
    src/misc.c       # Serial, RTC, CMOS

    # Sound devices
    src/adlib.c      # Adlib/OPL2
    src/sb16.c       # Sound Blaster 16
    src/pcspk.c      # PC Speaker
    src/sn76489.c    # Tandy 3-Voice Sound (SN76489)
    src/dss.c        # Disney Sound Source
    # FM OPL synthesis engine
    src/emu8950/emu8950.c
    src/emu8950/emuadpcm.c
    src/emu8950/slot_render.cpp

    # Optional devices (can be disabled for size)
    src/fpu.c      # FPU emulation - disabled for initial port
    # src/ne2000.c   # Network - disabled for initial port

    # Configuration
    src/ini.c
    src/profile_subsys.c
    src/codeprofile.c
    src/gameport.c
    $<$<STREQUAL:${BOARD},C2>:${CMAKE_CURRENT_LIST_DIR}/src/diskcache.c>
    src/pcsample.c
    src/bbprofile.c
    src/autotype.c
    $<$<STREQUAL:${BOARD},C2>:${CMAKE_CURRENT_LIST_DIR}/src/remote_mem.c>
    $<$<STREQUAL:${BOARD},C2>:${CMAKE_CURRENT_LIST_DIR}/link/link_fast.c>

    # access to sd-card using W/A with network-like driver (mapdrive.com)
    src/netredirect.c
)

#=============================================================================
# Main Executable
#=============================================================================
# the BIOS ROM: rom.asm assembled (nasm) into a C array
set(HLE_ROM_C ${CMAKE_BINARY_DIR}/gen/rom_bin.c)
add_custom_command(
    OUTPUT ${HLE_ROM_C}
    COMMAND ${CMAKE_COMMAND} -E env python3 ${CMAKE_CURRENT_LIST_DIR}/tools/mkrom.py
            ${CMAKE_CURRENT_LIST_DIR}/src/hle/rom.asm ${CMAKE_CURRENT_LIST_DIR}/src/hle/rom_layout.h ${HLE_ROM_C}
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tools/mkrom.py ${CMAKE_CURRENT_LIST_DIR}/src/hle/rom.asm
            ${CMAKE_CURRENT_LIST_DIR}/src/hle/rom_layout.h
    COMMENT "Assembling the BIOS ROM")

add_executable(${BUILD_NAME}
    src/main.c
    src/hle/boot.c
    src/hle/hle.c
    src/hle/int10.c
    src/hle/int13.c
    src/hle/kbd.c
    src/hle/misc.c
    src/hle/fonts/font8x8.c
    src/hle/fonts/font8x14.c
    src/hle/coro_rp2350.c
    src/hle/coro_rp2350.S
    src/fdos/bios_shim.c
    src/ezabi/dos_api.c
    src/ezabi/ezabi_real.c
    ${HLE_ROM_C}
    src/platform_rp2350.c
    src/stubs_rp2350.c
    src/diskui.c
    src/settingsui.c
    src/config_save.c
    src/ps2kbd_mrmltr.cpp
    src/ps2kbd_wrapper.cpp
    src/cxx_nothrow.cpp
    ${EMU_CORE_SOURCES}
)

target_include_directories(${BUILD_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs
    ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard
    ${CMAKE_CURRENT_LIST_DIR}/drivers/ps2
    ${CMAKE_CURRENT_LIST_DIR}/drivers/psram
    ${CMAKE_CURRENT_LIST_DIR}/drivers/vga
    ${CMAKE_CURRENT_LIST_DIR}/drivers/hdmi
    ${CMAKE_CURRENT_LIST_DIR}/drivers/audio
    ${CMAKE_CURRENT_LIST_DIR}/drivers/nespad
    ${CMAKE_CURRENT_LIST_DIR}/link
)
if(BOARD STREQUAL "C2")
    pico_generate_pio_header(${BUILD_NAME} ${CMAKE_CURRENT_LIST_DIR}/link/link_bus.pio)
endif()
# Only include usbhid directory when USB HID is enabled (to avoid tusb_config.h conflicts)
if(USB_HID_ENABLED)
    target_include_directories(${BUILD_NAME} PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/drivers/usbhid
    )
endif()

if(EMM)
    target_compile_definitions(${BUILD_NAME} PRIVATE EMULATE_LTEMS=1)
endif()
target_compile_definitions(${BUILD_NAME} PRIVATE
    # Version information
    FRANK_DOS_VERSION_MAJOR=${FRANK_DOS_VERSION_MAJOR}
    FRANK_DOS_VERSION_MINOR=${FRANK_DOS_VERSION_MINOR}

    # Board configuration
    BOARD_${BOARD}
    BOARD_VARIANT_STR="${BOARD}"
    CPU_CLOCK_MHZ=${CPU_SPEED}
    CPU_VOLTAGE=${CPU_VOLTAGE}
    PSRAM_MAX_FREQ_MHZ=${PSRAM_SPEED}
    FLASH_MAX_FREQ_MHZ=${FLASH_SPEED}

    # Platform identification
    PICO_ON_DEVICE=1
    RP2350_BUILD=1

    # Emulator configuration (8MB PSRAM)
    EMU_MEM_SIZE_MB=8
    EMU_VGA_MEM_SIZE_KB=256
    EMU_CPU_GEN=4

    # Disable features for initial port
#    NO_FPU=1
    # NO_SOUND=1  # Sound enabled with I2S output
    NO_NETWORK=1

    # Performance optimizations
    I386_ENABLE_INLINE=1

    SOUND_FREQUENCY=44100

    # OPL2
    USE_EMU8950_OPL
    EMU8950_SLOT_RENDER=1
    EMU8950_NO_RATECONV=1
    EMU8950_NO_WAVE_TABLE_MAP=1
    EMU8950_NO_TLL=1
    EMU8950_NO_FLOAT=1
    EMU8950_NO_TIMER=1
    EMU8950_NO_TEST_FLAG=1
    EMU8950_SIMPLER_NOISE=1
    EMU8950_SHORT_NOISE_UPDATE_CHECK=1
    EMU8950_LINEAR_SKIP=1
    EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION
    EMU8950_NO_PERCUSSION_MODE=1
    EMU8950_LINEAR=1
)

if(ADLIB_CORE1_DEF)
    target_compile_definitions(${BUILD_NAME} PRIVATE ${ADLIB_CORE1_DEF})
endif()

# Add DEBUG_ENABLED if enabled
if(DEBUG_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE DEBUG_ENABLED=1)
endif()

# Force HDMI output
if(FORCE_HDMI)
    target_compile_definitions(${BUILD_NAME} PRIVATE FORCE_HDMI=1)
    target_compile_definitions(video_driver PRIVATE FORCE_HDMI=1)
endif()

# Force VGA output (wins over FORCE_HDMI in vga_hw_init)
if(FORCE_VGA)
    target_compile_definitions(${BUILD_NAME} PRIVATE FORCE_VGA=1)
    target_compile_definitions(video_driver PRIVATE FORCE_VGA=1)
endif()

# Add I386_PROFILE if profiling enabled
if(PROFILE_ENABLED)
    target_compile_definitions(${BUILD_NAME} PRIVATE I386_PROFILE=1)
endif()

# Guest memory served by the slave RP2350 (C2 only). See src/remote_mem.h.
option(REMOTE_MEM "C2: serve a window of guest RAM from the slave's SRAM" OFF)

# Ignore cpu_freq/psram_freq from the SD card's config.ini. Needed for any
# clock-scaling measurement, since config.ini otherwise pins this board to
# 504 MHz regardless of what the build asked for.
option(PIN_CLOCKS "Ignore config.ini clock settings" OFF)

# Guest code footprint measurement, for JIT cache sizing. See codeprofile.h.
option(CODE_PROFILE "Measure the guest code working set" OFF)

# Statistical PC sampling of core 0 via SysTick. See src/pcsample.c.
# Scripted keystrokes for unattended benchmarking. See src/autotype.h.
set(AUTOTYPE "" CACHE STRING "Keys to type into the guest after boot")
if(NOT AUTOTYPE STREQUAL "")
    target_compile_definitions(${BUILD_NAME} PRIVATE AUTOTYPE_STR="${AUTOTYPE}")
endif()

option(PC_SAMPLE "Sample core-0 PC to find interpreter hot spots" OFF)

# Basic-block discovery and reuse profiling. Stage 1 of the JIT.
# SD block cache in the slave RP2350's PSRAM (C2 only). See src/diskcache.h.
# (FAST_FETCH is gone: frank-react's core reads instructions straight from
# guest memory, inline, on every board.)

option(DISK_CACHE "C2: cache SD blocks in the slave's PSRAM" OFF)
if(DISK_CACHE AND BOARD STREQUAL "C2")
    target_compile_definitions(${BUILD_NAME} PRIVATE DISK_CACHE=1)
    target_compile_definitions(sdcard PRIVATE DISK_CACHE=1)
endif()

option(BB_PROFILE "Profile guest basic blocks" OFF)
if(BB_PROFILE)
    target_compile_definitions(${BUILD_NAME} PRIVATE BB_PROFILE=1)
endif()
if(PC_SAMPLE)
    target_compile_definitions(${BUILD_NAME} PRIVATE PC_SAMPLE=1)
endif()
if(CODE_PROFILE)
    target_compile_definitions(${BUILD_NAME} PRIVATE CODE_PROFILE=1)
endif()
if(PIN_CLOCKS)
    target_compile_definitions(${BUILD_NAME} PRIVATE PIN_CLOCKS=1)
endif()
if(REMOTE_MEM AND BOARD STREQUAL "C2")
    target_compile_definitions(${BUILD_NAME} PRIVATE REMOTE_MEM=1)
endif()

# Subsystem cycle profiling. sdcard needs it too: the timed disk_read /
# disk_write wrappers live there.
if(SUBSYS_PROFILE)
    target_compile_definitions(${BUILD_NAME} PRIVATE SUBSYS_PROFILE=1)
    target_compile_definitions(sdcard PRIVATE SUBSYS_PROFILE=1)
endif()

# Compiler optimizations for performance
target_compile_options(${BUILD_NAME} PRIVATE
    -O3
    -ffunction-sections
    -fdata-sections
)

# SRAM holds cpu_exec1 (the interpreter real-mode and 16-bit code runs on) or
# the fast loop and its kernel, not both: ON = cpu_exec1 (default; the fast
# loop is then not run at all: from flash it was slower than cpu_exec1 alone,
# see fastloop.inc), OFF = frank-react's layout, the loop in SRAM.
option(CPU_EXEC1_SRAM "cpu_exec1 in SRAM, no fast loop" ON)
if(CPU_EXEC1_SRAM)
    target_compile_definitions(${BUILD_NAME} PRIVATE CPU_EXEC1_SRAM=1 FX_SRAM=0)
else()
    target_compile_definitions(${BUILD_NAME} PRIVATE CPU_EXEC1_SRAM=0 FX_SRAM=1)
endif()
# The real-mode JIT (src/jit16.inc): blocks of 16-bit code as calls to
# per-instruction helpers, from a code cache in SRAM. Needs cpu_exec1 in SRAM.
option(CPU_JIT16 "real-mode JIT (src/jit16.inc)" OFF)
option(CPU_JIT16_STATS "the JIT: its counters on the board (jit_stat_*)" OFF)
option(CPU_JIT16_PROF "the JIT: count the interpreter's real-mode opcodes (jit_prof_op)" OFF)
if(CPU_JIT16 AND CPU_EXEC1_SRAM)
    target_compile_definitions(${BUILD_NAME} PRIVATE JIT16=1)
    if(BOARD STREQUAL "NX")
        # The CPU's SRAM as an overlay chosen at boot by the jit setting
        # (config.ini jit=0|1): the classic interpreter, or the compact one
        # with the JIT and a cache that fills the rest of the same SRAM.
        pico_set_linker_script(${BUILD_NAME} ${CMAKE_CURRENT_LIST_DIR}/boards/memmap_nx_overlay.ld)
        target_compile_definitions(${BUILD_NAME} PRIVATE EXEC1_OVERLAY=1 JIT_CACHE_BYTES=49152)
    endif()
    if(CPU_JIT16_STATS)     # (the per-block-run counters, read over SWD)
        target_compile_definitions(${BUILD_NAME} PRIVATE JIT_STATS=1)
    endif()
    if(CPU_JIT16_PROF)
        # (its tables: room from the code cache)
        target_compile_definitions(${BUILD_NAME} PRIVATE JIT_PROF=1 JIT_CACHE_BYTES=61440)
    endif()
endif()
# frank-dos: newlib-nano's number conversions are weak (NULL unless something
# references them); the native application ABI's vsscanf (slot 102) needs them
target_link_options(${BUILD_NAME} PRIVATE -Wl,-u,_scanf_i -Wl,-u,_scanf_float -Wl,-u,_printf_float)
# frank-dos: the FreeDOS kernel's data in PSRAM needs its linker rules on
# every board (NX has its own script, with the CPU overlay)
if(NOT BOARD STREQUAL "NX" OR NOT (CPU_JIT16 AND CPU_EXEC1_SRAM))
    pico_set_linker_script(${BUILD_NAME} ${CMAKE_CURRENT_LIST_DIR}/boards/memmap_m2.ld)
endif()
# The i386 core's fast loop (fastloop.inc) and its Thumb-2 kernel
# (fastloop_m33.S), from frank-react. -DFX_KERNEL=OFF: the C fast loop alone.
option(FX_KERNEL "the fast loop's Thumb-2 kernel" ON)
if(FX_KERNEL AND NOT CPU_EXEC1_SRAM)
    target_sources(${BUILD_NAME} PRIVATE src/fastloop_m33.S)
    target_compile_definitions(${BUILD_NAME} PRIVATE FX_KERNEL=1)
endif()
set_source_files_properties(src/fastloop_m33.S PROPERTIES
    COMPILE_FLAGS "-mcpu=cortex-m33 -mthumb"
)

#=============================================================================
# The FreeDOS kernel, native (DnCraptor's port, src/fdos) and its runtime
#=============================================================================
set(FDOS_SOURCES
    src/fdos/boot.c src/fdos/intr.c src/fdos/initclk.c src/fdos/initdisk.c src/fdos/dyninit.c
    src/fdos/config.cpp src/fdos/memmgr.cpp src/fdos/inithma.c src/fdos/dsk.cpp src/fdos/dosfns.cpp
    src/fdos/fcbfns.c src/fdos/lfnapi.c src/fdos/lfn_guest_proxy.cpp src/fdos/fatfs.cpp
    src/fdos/fattab.cpp src/fdos/fatdir.cpp src/fdos/blockio.cpp src/fdos/chario.c src/fdos/kernel.c
    src/fdos/native_devices.cpp src/fdos/kernel_guest_proxy.cpp src/fdos/mcb_proxy.cpp src/fdos/task.c
    src/fdos/fcom/fcom.c src/fdos/fcom/fcom_guest.cpp src/fdos/nls.c src/fdos/ioctl.c
    src/fdos/ioctl_guest_proxy.cpp src/fdos/fdos_21h.cpp src/fdos/fdos_2528.c src/fdos/fdos_29h.c
    src/fdos/fdos_2fh.c
    src/fdos/dosrt.c
    src/fdos/dosdbg.c
    src/fdos/sddrive.c
)
add_library(fdos STATIC ${FDOS_SOURCES})
set_source_files_properties(src/fdos/fcom/fcom.c PROPERTIES LANGUAGE CXX)
target_include_directories(fdos BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src/fdos/compat
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/src/fdos
    ${CMAKE_CURRENT_LIST_DIR}/src/fdos/hdr
    $<TARGET_PROPERTY:${BUILD_NAME},INCLUDE_DIRECTORIES>)
target_compile_definitions(fdos PRIVATE
    $<TARGET_PROPERTY:${BUILD_NAME},COMPILE_DEFINITIONS>
    WITHFAT32=1 WITHLFNAPI=1 I386_MODE=1 EMU_CPU_GEN=3
    SD_DATA_DIR="dos" SD_DATA_DIR_SLASH="dos/")
target_compile_options(fdos PRIVATE -O2 -ffunction-sections -fdata-sections -Wno-unused-parameter -Wno-sign-compare
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -std=gnu++17>)
# (headers only: an INTERFACE library's sources would be compiled into libfdos too)
target_link_libraries(fdos PRIVATE pico_stdlib_headers hardware_pio_headers hardware_gpio_headers
    pico_time_headers hardware_sync_headers)
target_include_directories(fdos PRIVATE ${CMAKE_CURRENT_LIST_DIR}/drivers/fatfs ${CMAKE_CURRENT_LIST_DIR}/drivers/sdcard)

target_link_libraries(${BUILD_NAME}
    fdos
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_flash
    hardware_watchdog
    hardware_irq
    hardware_interp
    fatfs
    sdcard
    ps2_driver
    psram
    video_driver
    audio_driver
    nespad
    usbhid
)

# USB configuration: USB HID uses Host mode which disables CDC
if(USB_HID_ENABLED)
    pico_enable_stdio_usb(${BUILD_NAME} 0)
    if(BOARD STREQUAL "C2")
        # C2 has a dedicated debug UART header (J2: GPIO0 TX, GPIO1 RX,
        # 115200). With USB in host mode it is the only console there is,
        # and bring-up on a board whose slave cannot be reset by the
        # master is not something to attempt blind.
        message(STATUS "USB HID enabled: console on UART0 (J2, GPIO0/1)")
        pico_enable_stdio_uart(${BUILD_NAME} 1)
    elseif(BOARD STREQUAL "NX")
        # FRANK Next: J2 (GPIO46 TX, GPIO45 RX, 115200), as C2's J2.
        message(STATUS "USB HID enabled: console on UART0 (J2, GPIO46/45)")
        pico_enable_stdio_uart(${BUILD_NAME} 1)
    elseif(BOARD STREQUAL "Z2")
        # Z2's GPIO0/1 carry the PS/2 mouse, which board_config.h drops
        # in a USB HID build, so the UART console can have them.
        message(STATUS "USB HID enabled: console on UART0 (GPIO0/1)")
        pico_enable_stdio_uart(${BUILD_NAME} 1)
    else()
        # Other boards route GPIO0/1 to PS/2 or a gamepad, so there is no
        # UART to fall back on — stdio is simply off.
        message(STATUS "USB HID enabled: no console (USB is Host mode for keyboard)")
        pico_enable_stdio_uart(${BUILD_NAME} 0)
    endif()
else()
    # USB Serial console (when USB HID is disabled)
    pico_enable_stdio_usb(${BUILD_NAME} 1)
    if(BOARD STREQUAL "NX")
        pico_enable_stdio_uart(${BUILD_NAME} 1)     # J2 as well (output)
    else()
        pico_enable_stdio_uart(${BUILD_NAME} 0)
    endif()
endif()

#=============================================================================
# Print configuration summary
#=============================================================================
message(STATUS "")
message(STATUS "frank-386 Build Configuration:")
message(STATUS "  Board Variant:  ${BOARD}")
message(STATUS "  EMM:            ${EMM}")
message(STATUS "  CPU Speed:      ${CPU_SPEED} MHz")
message(STATUS "  CPU Voltage:    ${CPU_VOLTAGE}")
message(STATUS "  PSRAM Speed:    ${PSRAM_SPEED} MHz")
message(STATUS "  Audio Type:     runtime AUTO/PWM/I2S")
message(STATUS "  USB HID:        ${USB_HID_ENABLED}")
message(STATUS "  Debug Output:   ${DEBUG_ENABLED}")
message(STATUS "  Build Name:     ${BUILD_NAME}")
message(STATUS "")

# Create UF2 output
pico_add_extra_outputs(${BUILD_NAME})

target_link_options(${BUILD_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections -Wl,--gc-sections)
