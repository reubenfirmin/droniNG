/* SPDX-License-Identifier: GPL-3.0-or-later
 * Deliberately bounded host fixture, not a firmware compatibility ABI.
 * MPU enums/IDs come from the untouched upstream header. Device structures
 * and services below model only the fields used by the selected chip driver.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "pios_spi.h"

typedef struct {
    pios_spi_t spi;
    uint32_t slave;
    uint32_t bus_hz;
} extDevice_t;
typedef int busStatus_e;
#include "drivers/accgyro/accgyro_mpu.h"

typedef struct gyroDev_s gyroDev_t;
typedef struct accDev_s accDev_t;
struct gyroDev_s {
    extDevice_t dev;
    mpuDetectionResult_t mpuDetectionResult;
    void (*initFn)(gyroDev_t *);
    bool (*readFn)(gyroDev_t *);
    float scale, tempScale, tempZero;
    uint8_t accDataReg, gyroDataReg, tempDataReg, dmaReadRegStart;
    unsigned mpuDividerDrops, gyroRateKHz;
    int16_t raw[3];
};
struct accDev_s {
    gyroDev_t *gyro;
    mpuDetectionResult_t mpuDetectionResult;
    void (*initFn)(accDev_t *);
    bool (*readFn)(accDev_t *);
    unsigned acc_1G;
    int16_t raw[3];
};
#define GYRO_SCALE_2000DPS (2000.0f / 32768.0f)
#define GYRO_SCALE_4000DPS (4000.0f / 32768.0f)
enum { GYRO_HARDWARE_LPF_NORMAL, GYRO_HARDWARE_LPF_OPTION_1, GYRO_HARDWARE_LPF_OPTION_2 };
enum { GYRO_RATE_1_kHz = 0, GYRO_RATE_8_kHz = 5 };
typedef struct { uint8_t gyro_hardware_lpf; } gyroConfig_t;
const gyroConfig_t *gyroConfig(void);
void delay(unsigned ms);
static inline uint32_t llog2(uint32_t n) { return 31 - __builtin_clz(n | 1); }
#define STATIC_UNIT_TESTED static

void spiWriteReg(const extDevice_t *, uint8_t, uint8_t);
uint8_t spiReadRegMsk(const extDevice_t *, uint8_t);
uint16_t spiCalculateDivider(uint32_t);
void spiSetClkDivisor(const extDevice_t *, uint16_t);

/* Only used in the second, external-clock-enabled build. */
typedef unsigned ioTag_t;
typedef unsigned IO_t;
#define GYRO_COUNT 1
#define RESOURCE_INDEX(i) ((i) + 1)
extern struct surveyGyro { struct { gyroDev_t gyroDev; } gyroSensor[GYRO_COUNT]; } gyro;
struct gyroDeviceConfig_s { ioTag_t clkIn; };
const struct gyroDeviceConfig_s *gyroDeviceConfig(unsigned);
IO_t IOGetByTag(ioTag_t);
bool gyroClkInInit(ioTag_t, uint32_t, unsigned);
