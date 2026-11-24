# FRANK DOS on the host (macOS/Linux, SDL2): the same CPU core, devices,
# native BIOS and FreeDOS kernel as the firmware, for debugging DOS without
# the board.
#
#   cmake -S host -B build-host && make -C build-host -j
#   build-host/frankdos-host [sd-root]      (sd-root: the SD card's files)
cmake_minimum_required(VERSION 3.13)
project(frankdos_host C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(TOP ${CMAKE_CURRENT_LIST_DIR}/..)
find_package(SDL2 REQUIRED)

set(HLE_ROM_C ${CMAKE_BINARY_DIR}/gen/rom_bin.c)
add_custom_command(
    OUTPUT ${HLE_ROM_C}
    COMMAND python3 ${TOP}/tools/mkrom.py ${TOP}/src/hle/rom.asm ${TOP}/src/hle/rom_layout.h ${HLE_ROM_C}
    DEPENDS ${TOP}/tools/mkrom.py ${TOP}/src/hle/rom.asm ${TOP}/src/hle/rom_layout.h
    COMMENT "Assembling the BIOS ROM")

# the 8x16 font, without the board's section placement (not valid in Mach-O)
set(HOST_FONT_C ${CMAKE_BINARY_DIR}/gen/font8x16.c)
file(READ ${TOP}/drivers/vga/font8x16.c _font)
string(REPLACE "__attribute__((section(\".data\")))" "" _font "${_font}")
file(WRITE ${HOST_FONT_C} "${_font}")

set(COMMON_DEFS
    FRANK_HOST=1
    FRANK_DOS_VERSION_MAJOR=0 FRANK_DOS_VERSION_MINOR=1
    FRANK_PSRAM_BASE=0x400000000000ull
    PICO_ON_DEVICE=0
    BOARD_NX BOARD_VARIANT_STR="HOST"
    EMU_MEM_SIZE_MB=8 EMU_VGA_MEM_SIZE_KB=256
    NO_NETWORK=1 I386_ENABLE_INLINE=1 SOUND_FREQUENCY=44100
    JIT16=0
    USE_EMU8950_OPL EMU8950_SLOT_RENDER=1 EMU8950_NO_RATECONV=1 EMU8950_NO_WAVE_TABLE_MAP=1
    EMU8950_NO_TLL=1 EMU8950_NO_FLOAT=1 EMU8950_NO_TIMER=1 EMU8950_NO_TEST_FLAG=1
    EMU8950_SIMPLER_NOISE=1 EMU8950_SHORT_NOISE_UPDATE_CHECK=1 EMU8950_LINEAR_SKIP=1
    EMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION EMU8950_NO_PERCUSSION_MODE=1 EMU8950_LINEAR=1)

set(COMMON_INCS
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${TOP}
    ${TOP}/src
    ${TOP}/drivers/fatfs
    ${TOP}/drivers/vga)

set(COMMON_WARN -include ${CMAKE_CURRENT_LIST_DIR}/include/host_compat.h -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -Wno-unused-parameter -Wno-sign-compare -Wno-pointer-to-int-cast
    -Wno-int-to-pointer-cast -Wno-unused-function -Wno-unused-variable)

# the FreeDOS kernel, as the firmware builds it
set(FDOS_DIR ${TOP}/src/fdos)
set(FDOS_SOURCES
    boot.c intr.c initclk.c initdisk.c dyninit.c config.cpp memmgr.cpp inithma.c dsk.cpp dosfns.cpp
    fcbfns.c lfnapi.c lfn_guest_proxy.cpp fatfs.cpp fattab.cpp fatdir.cpp blockio.cpp chario.c kernel.c
    native_devices.cpp kernel_guest_proxy.cpp mcb_proxy.cpp task.c fcom/fcom.c fcom/fcom_guest.cpp
    nls.c ioctl.c ioctl_guest_proxy.cpp fdos_21h.cpp fdos_2528.c fdos_29h.c fdos_2fh.c dosrt.c dosdbg.c sddrive.c)
list(TRANSFORM FDOS_SOURCES PREPEND ${FDOS_DIR}/)
add_library(fdos STATIC ${FDOS_SOURCES})
set_source_files_properties(${FDOS_DIR}/fcom/fcom.c PROPERTIES LANGUAGE CXX)
target_include_directories(fdos BEFORE PRIVATE
    ${FDOS_DIR}/compat ${TOP}/src ${FDOS_DIR} ${FDOS_DIR}/hdr ${COMMON_INCS})
target_compile_definitions(fdos PRIVATE ${COMMON_DEFS}
    WITHFAT32=1 WITHLFNAPI=1 I386_MODE=1 EMU_CPU_GEN=3 SD_DATA_DIR="dos" SD_DATA_DIR_SLASH="dos/"
    # the kernel's read/close/lseek (its DOS file calls) would take libc's names in the host's executable
    read=fdos_read close=fdos_close lseek=fdos_lseek)
target_compile_options(fdos PRIVATE ${COMMON_WARN}
    -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -std=gnu++17 -Wno-c++11-narrowing>)

add_executable(frankdos-host
    ${TOP}/src/i386.c ${TOP}/src/pc.c
    ${TOP}/src/i8042.c ${TOP}/src/i8254.c ${TOP}/src/i8257.c ${TOP}/src/i8259.c
    ${TOP}/src/vga.c ${TOP}/src/disk.c ${TOP}/src/ide.c ${TOP}/src/fdd.c ${TOP}/src/pci.c ${TOP}/src/misc.c
    ${TOP}/src/adlib.c ${TOP}/src/sb16.c ${TOP}/src/pcspk.c ${TOP}/src/sn76489.c ${TOP}/src/dss.c
    ${TOP}/src/emu8950/emu8950.c ${TOP}/src/emu8950/emuadpcm.c ${TOP}/src/emu8950/slot_render.cpp
    ${TOP}/src/fpu.c ${TOP}/src/ini.c ${TOP}/src/gameport.c ${TOP}/src/autotype.c ${TOP}/src/netredirect.c
    ${TOP}/src/hle/boot.c ${TOP}/src/hle/hle.c ${TOP}/src/hle/int10.c ${TOP}/src/hle/int13.c
    ${TOP}/src/hle/kbd.c ${TOP}/src/hle/misc.c
    ${TOP}/src/hle/fonts/font8x8.c ${TOP}/src/hle/fonts/font8x14.c
    ${TOP}/src/fdos/bios_shim.c
    ${TOP}/src/ezabi/dos_api.c
    ${TOP}/src/ezabi/ezabi_real.c
    ${CMAKE_CURRENT_LIST_DIR}/ezapp_host.c
    ${CMAKE_CURRENT_LIST_DIR}/aeabi_host.c
    ${HLE_ROM_C}
    ${HOST_FONT_C}
    ${TOP}/drivers/fatfs/ffunicode.c
    ${CMAKE_CURRENT_LIST_DIR}/main.c
    ${CMAKE_CURRENT_LIST_DIR}/platform.c
    ${CMAKE_CURRENT_LIST_DIR}/ff_host.c
    ${CMAKE_CURRENT_LIST_DIR}/host_dir.c
    ${CMAKE_CURRENT_LIST_DIR}/coro_host.c)
target_include_directories(frankdos-host PRIVATE ${COMMON_INCS} ${SDL2_INCLUDE_DIRS})
target_compile_definitions(frankdos-host PRIVATE ${COMMON_DEFS})
target_compile_options(frankdos-host PRIVATE ${COMMON_WARN})
target_link_libraries(frankdos-host PRIVATE fdos ${SDL2_LIBRARIES} m)
