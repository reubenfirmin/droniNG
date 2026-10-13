/* SPDX-License-Identifier: GPL-3.0-or-later
 * Bounded services for the unchanged RADIX FPGA source. Host/compile probe only.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pios_spi.h"

typedef uint16_t IO_t;
typedef struct {
    struct { struct { IO_t csnPin; } spi; } busType_u;
    pios_spi_t spi;
    uint32_t hz;
} extDevice_t;

#define PA8 0x18
#define PB1 0x21
#define PB12 0x2c
#define PC4 0x34
#define PC15 0x3f
#define SPI3 3
#define IO_NONE 0
#define IO_TAG(pin) (pin)
#define OWNER_OSD 1
#define SPI_IO_CS_CFG 1
#define IOCFG_IN_FLOATING 2
#define IOCFG_AF_PP 3
#define GPIO_OType_OD 4
#define GPIO_Mode_OUT 5
#define GPIO_Speed_2MHz 6
#define GPIO_OType_PP 7
#define GPIO_PuPd_DOWN 8
#define GPIO_AF_MCO 9
#define IO_CONFIG(mode, speed, type, pull) ((mode) | (speed) | (type) | (pull))
#define SPI_DEV_TO_CFG(bus) (bus)
#define RCC_MCO1Source_HSE 1
#define RCC_MCO1Div_1 1
#define ENABLE 1
#define MIN(a, b) ((a) < (b) ? (a) : (b))

int spiDeviceByInstance(int instance);
bool spiSetBusInstance(extDevice_t *dev, int bus);
void spiSetClkDivisor(extDevice_t *dev, uint16_t divisor);
void spiWriteReg(const extDevice_t *dev, uint8_t reg, uint8_t data);
void spiWriteRegBuf(const extDevice_t *dev, uint8_t reg, const uint8_t *data, unsigned len);
IO_t IOGetByTag(uint16_t tag);
void IOInit(IO_t pin, unsigned owner, unsigned index);
void IOConfigGPIO(IO_t pin, unsigned config);
void IOConfigGPIOAF(IO_t pin, unsigned config, unsigned af);
void IOHi(IO_t pin);
void IOLo(IO_t pin);
bool IORead(IO_t pin);
void delay(unsigned ms);
void RCC_MCO1Config(unsigned source, unsigned divider);
void RCC_MCO1Cmd(unsigned enable);
