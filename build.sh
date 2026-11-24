#!/bin/bash
# Build frank-386 - 386 Emulator for RP2350
#
# Usage: ./build.sh [OPTIONS]
#   -b, --board      Board variant: M1, M2, PC, Z2, C2, NX (default: M2)
#   -a, --audio      Audio output: I2S, PWM (default: PWM; PC is always PWM)
#   -p, --psram      PSRAM speed in MHz (default: 133)
#   -c, --cpu        CPU speed in MHz: 378 (default), 504
#   --vga            Force VGA output (instead of HDMI)
#   --usb-hid        Enable USB HID keyboard (disables USB CDC)
#   --usb-cdc        NX: USB CDC console on the master's port (no USB keyboard)
#   --slave-cache    NX: SD sectors cached in the slave (slave: frank-386-sdcache)
#   --jit / --no-jit the real-mode JIT (src/jit16.inc); NX builds it by default
#   --debug          Enable debug output
#   -clean           Clean build directory first
#   -h, --help       Show this help
#
# Short options:
#   -M1, -M2, -PC, -Z2, -C2, -NX   Board variant
#   -378, -504            CPU speed in MHz
#   -i2s, -pwm            Audio output type

# Defaults (378/133 for stable overclocked operation)
BOARD="M2"
AUDIO="PWM"
PSRAM="133"
CPU="378"
USB_HID="OFF"
HDMI="OFF"
VGA="OFF"
DEBUG="ON"
PROFILE="OFF"
SUBSYS="OFF"
REMOTE="OFF"
PINCLK="OFF"
CODEPROF="OFF"
PCSAMPLE="OFF"
BBPROF="OFF"
DISKCACHE="OFF"
AUTOTYPE=""
NX_USB_CDC="OFF"
NX_SLAVE_CACHE="OFF"
CPU_JIT16=""
CLEAN=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -b|--board)
            BOARD="$2"
            shift 2
            ;;
        -M1)
            BOARD="M1"
            shift
            ;;
        -M2)
            BOARD="M2"
            shift
            ;;
        -PC)
            BOARD="PC"
            shift
            ;;
        -Z2)
            BOARD="Z2"
            shift
            ;;
        -C2)
            BOARD="C2"
            shift
            ;;
        -NX)
            BOARD="NX"
            shift
            ;;
        -a|--audio)
            AUDIO=$(echo "$2" | tr '[:lower:]' '[:upper:]')
            shift 2
            ;;
        -i2s)
            AUDIO="I2S"
            shift
            ;;
        -pwm)
            AUDIO="PWM"
            shift
            ;;
        -p|--psram)
            PSRAM="$2"
            shift 2
            ;;
        -c|--cpu)
            CPU="$2"
            shift 2
            ;;
        -378)
            CPU="378"
            PSRAM="133"
            shift
            ;;
        -504)
            CPU="504"
            PSRAM="166"
            shift
            ;;
        --usb-hid)
            USB_HID="ON"
            shift
            ;;
        --slave-cache)
            # NX: SD groups cached in the slave (flash slave/ frank-386-sdcache)
            NX_SLAVE_CACHE="ON"
            shift
            ;;
        --jit)
            CPU_JIT16="ON"
            shift
            ;;
        --no-jit)
            CPU_JIT16="OFF"
            shift
            ;;
        --usb-cdc)
            # NX: the master's USB port as the console (bench builds)
            NX_USB_CDC="ON"
            shift
            ;;
        --hdmi)
            HDMI="ON"
            VGA="OFF"
            shift
            ;;
        --vga)
            VGA="ON"
            HDMI="OFF"
            shift
            ;;
        --debug)
            DEBUG="ON"
            shift
            ;;
        --profile)
            PROFILE="ON"
            shift
            ;;
        --subsys-profile)
            SUBSYS="ON"
            shift
            ;;
        --remote-mem)
            REMOTE="ON"
            shift
            ;;
        --pin-clocks)
            PINCLK="ON"
            shift
            ;;
        --code-profile)
            CODEPROF="ON"
            shift
            ;;
        --pc-sample)
            PCSAMPLE="ON"
            shift
            ;;
        --bb-profile)
            BBPROF="ON"
            shift
            ;;
        --disk-cache)
            DISKCACHE="ON"
            shift
            ;;
        --autotype)
            AUTOTYPE="$2"
            shift 2
            ;;
        -clean)
            CLEAN=1
            shift
            ;;
        -h|--help)
            head -15 "$0" | tail -13
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Olimex PC has no I2S hardware - force PWM
if [[ "$BOARD" == "PC" && "$AUDIO" != "PWM" ]]; then
    echo "Warning: Olimex PC does not support I2S, forcing PWM audio"
    AUDIO="PWM"
fi

# FRANK Core 2: TDA1387T I2S DAC, HDMI only, USB HID is the only input.
# CMakeLists forces these too; set them here so the echoed summary is honest.
# FRANK Next: TLV320DAC3100 codec on I2S, likewise HDMI and USB HID only.
if [[ "$BOARD" == "C2" || "$BOARD" == "NX" ]]; then
    AUDIO="I2S"
    USB_HID="ON"
    [[ "$NX_USB_CDC" == "ON" ]] && USB_HID="OFF"
    [[ "$VGA" != "ON" ]] && HDMI="ON"
fi
# Flash (XIP) clock: FRANK Next's part holds 84 MHz (504/6; 100.8 fails even
# with the longest read delay); CheckIt's Whetstones +8% over 63 MHz. Other
# boards keep the default. config.ini's flash_freq still overrides it.
FLASH="66"
[[ "$BOARD" == "NX" ]] && FLASH="84"
# The JIT: in NX builds unless --no-jit (config.ini jit=0 still picks the
# classic interpreter at boot); elsewhere only with --jit.
if [[ -z "$CPU_JIT16" ]]; then
    CPU_JIT16="OFF"
    [[ "$BOARD" == "NX" ]] && CPU_JIT16="ON"
fi

# Build cmake arguments
CMAKE_ARGS=(-DPICO_BOARD=pico2 -DCMAKE_BUILD_TYPE=MinSizeRel)
CMAKE_ARGS+=("-DBOARD=${BOARD}")
CMAKE_ARGS+=("-DCPU_SPEED=$CPU")
CMAKE_ARGS+=("-DPSRAM_SPEED=$PSRAM")
CMAKE_ARGS+=("-DFLASH_SPEED=$FLASH")
CMAKE_ARGS+=("-DAUDIO_TYPE=$AUDIO")

CMAKE_ARGS+=("-DUSB_HID_ENABLED=$USB_HID")

# NOTE: every optional feature below is passed explicitly as ON or OFF.
# Omitting the flag leaves whatever CMake cached from the previous
# configure, so `./build.sh -C2` after `./build.sh -C2 --remote-mem`
# would quietly still have remote memory compiled in — which invalidates
# any before/after measurement taken with it.
CMAKE_ARGS+=("-DDEBUG_ENABLED=$DEBUG")

CMAKE_ARGS+=("-DPROFILE_ENABLED=$PROFILE")

CMAKE_ARGS+=("-DSUBSYS_PROFILE=$SUBSYS")

CMAKE_ARGS+=("-DREMOTE_MEM=$REMOTE")
CMAKE_ARGS+=("-DPIN_CLOCKS=$PINCLK")
CMAKE_ARGS+=("-DCODE_PROFILE=$CODEPROF")
CMAKE_ARGS+=("-DPC_SAMPLE=$PCSAMPLE")
CMAKE_ARGS+=("-DBB_PROFILE=$BBPROF")
CMAKE_ARGS+=("-DDISK_CACHE=$DISKCACHE")
CMAKE_ARGS+=("-DAUTOTYPE=$AUTOTYPE")

CMAKE_ARGS+=("-DFORCE_HDMI=$HDMI")
CMAKE_ARGS+=("-DNX_USB_CDC=$NX_USB_CDC")
CMAKE_ARGS+=("-DNX_SLAVE_CACHE=$NX_SLAVE_CACHE")
CMAKE_ARGS+=("-DCPU_JIT16=$CPU_JIT16")

CMAKE_ARGS+=("-DFORCE_VGA=$VGA")

echo "Building frank-386:"
echo "  Board: $BOARD"
echo "  Audio: $AUDIO"
echo "  CPU: $CPU MHz"
echo "  PSRAM: $PSRAM MHz"
echo "  USB HID: $USB_HID"
echo "  HDMI: $HDMI"
echo "  VGA: $VGA"
echo "  Debug: $DEBUG"
echo "  Profile: $PROFILE"
echo "  Subsys profile: $SUBSYS"
echo "  Remote mem: $REMOTE"
echo ""

if [[ $CLEAN -eq 1 ]] || [[ ! -d ./build ]]; then
    rm -rf ./build
    mkdir build
fi

cd build
cmake "${CMAKE_ARGS[@]}" .. || exit 1
# no stale image to flash after a failed build (frank-react's build.sh)
rm -f lib/*.uf2 *.elf
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 8)
