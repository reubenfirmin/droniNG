/* SPDX-License-Identifier: GPL-3.0-or-later; host hardware substitutes. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define PIOS_INCLUDE_MAX7456
#define PIOS_INCLUDE_MPU
#define PIOS_INCLUDE_SPI
#define PIOS_Assert assert
typedef uintptr_t pios_i2c_t;
#include <pios_spi.h>
#ifdef PIOS_INCLUDE_BMP280_SPI
#include <pios_delay.h>
#include <pios_sensors.h>
#endif
void *PIOS_malloc(size_t size);
void PIOS_free(void *memory);
