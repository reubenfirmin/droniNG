/* SPDX-License-Identifier: GPL-3.0-or-later; bounded host service declarations. */
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define PIOS_INCLUDE_I2C
#define PIOS_INCLUDE_MPU6050_REVISION
#define PIOS_Assert assert
#define NELEMENTS(x) (sizeof(x) / sizeof((x)[0]))
#define USE_MPU_DATA_READY_SIGNAL

/* Services used by the original chip header/source, not its acquisition loop. */
typedef void (*sensorGyroUpdateFuncPtr)(void);
typedef struct mock_i2c I2C_TypeDef;
typedef uint16_t ioTag_t;
typedef uint32_t rccPeriphTag_t;
#include "accgyro_mpu.h"
typedef struct accDev_s {
    mpuDetectionResult_t mpuDetectionResult;
    void (*init)(struct accDev_s *);
    bool (*read)(struct accDev_s *);
    uint16_t acc_1G;
    char revisionCode;
} accDev_t;
typedef struct gyroDev_s {
    mpuDetectionResult_t mpuDetectionResult;
    mpuConfiguration_t mpuConfiguration;
    void (*init)(struct gyroDev_s *);
    bool (*read)(struct gyroDev_s *);
    bool (*intStatus)(struct gyroDev_s *);
    uint8_t lpf;
    float scale;
} gyroDev_t;
uint8_t gyroMPU6xxxGetDividerDrops(const gyroDev_t *gyro);
void delay(uint32_t milliseconds);
