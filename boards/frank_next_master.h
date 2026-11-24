// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
//
// FRANK Next (rev 1.00) — master half (U3, RP2350B in QFN-80).
//
// Used by BOARD=NX. From frank-test's boards/frank_next.h. The link to the
// slave (GPIO20..42) and HDMI (GPIO12..19) are the Core 2's; what differs:
//
//   - the console is UART0 on J2, TX GPIO46 / RX GPIO45. GPIO0/1 are the
//     microSD's CMD and CLK here, so frank_core2_master.h would drive the
//     card's command line with every printf. GPIO46 is UART0 TX only at the
//     B package's alternate function; the SDK picks it per pin;
//   - the microSD is 4-bit SDIO: CMD GPIO0, CLK GPIO1, DAT0-3 GPIO4-7;
//   - I2C1 on GPIO2 (SDA) / GPIO3 (SCL): TLV320DAC3100 (0x18), DS3231 (0x68);
//   - no LED on this half (both are on the slave).
#ifndef _BOARDS_FRANK_NEXT_MASTER_H
#define _BOARDS_FRANK_NEXT_MASTER_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define FRANK_NEXT_MASTER

// --- RP2350 VARIANT ---
// RP2350B: 48 GPIOs, so PIO needs the movable 32-pin window.
#define PICO_RP2350A 0

pico_board_cmake_set_default(PICO_PIO_USE_GPIO_BASE, 1)
#ifndef PICO_PIO_USE_GPIO_BASE
#define PICO_PIO_USE_GPIO_BASE 1
#endif

// --- UART (J2 header: pin 1 = TX, pin 2 = GND, pin 3 = RX) ---
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 46
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 45
#endif

// --- I2C (codec and RTC) ---
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 2
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 3
#endif

// --- FLASH (W25Q128JVPIQ, 16 MB) ---
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
