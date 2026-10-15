/* SPDX-License-Identifier: GPL-3.0-or-later; actual board caller and MPU initializer. */
#include <pios.h>
#include <pios_mpu.h>
#include <setjmp.h>
#include <stdio.h>

struct pios_mpu_dev {
    enum pios_mpu_type mpu_type;
    enum { PIOS_MPU_COM_SPI } com_driver_type;
    pios_spi_t spi_driver_id;
    uint32_t com_slave_addr;
    const struct pios_mpu_cfg *cfg;
};
static struct pios_mpu_dev allocated, *mpu_dev;
static const struct pios_mpu_cfg imu_cfg = { .default_samplerate = 1000 };
static enum pios_mpu_type detected_type;
static int allocation_calls, config_calls, setter_calls, panic;
static int probe_error, config_error, setter_error;
static bool allocation_error;
static jmp_buf failed;
#define PIOS_LED_ALARM 1
#define PIOS_HAL_PANIC_IMU 2

static struct pios_mpu_dev *PIOS_MPU_Alloc(const struct pios_mpu_cfg *cfg)
{
    assert(cfg == &imu_cfg);
    ++allocation_calls;
    memset(&allocated, 0, sizeof(allocated));
    return allocation_error ? NULL : &allocated;
}
static int32_t PIOS_MPU_SPI_Probe(enum pios_mpu_type *type)
{
    *type = detected_type;
    return probe_error;
}
static int32_t PIOS_MPU_Common_Init(void)
{
    assert(mpu_dev == &allocated);
    assert(mpu_dev->cfg == &imu_cfg);
    assert(mpu_dev->spi_driver_id == (pios_spi_t)1 && mpu_dev->com_slave_addr == 0);
    ++config_calls;
    return config_error;
}
enum pios_mpu_type PIOS_MPU_GetType(void) { return mpu_dev->mpu_type; }
int32_t PIOS_MPU_SetGyroRange(enum pios_mpu_gyro_range range)
{
    assert(range == PIOS_MPU_SCALE_2000_DEG);
    ++setter_calls; return setter_error == 1 ? -1 : 0;
}
int32_t PIOS_MPU_SetAccelRange(enum pios_mpu_accel_range range)
{
    assert(range == PIOS_MPU_SCALE_8G);
    ++setter_calls; return setter_error == 2 ? -1 : 0;
}
int32_t PIOS_MPU_SetGyroBandwidth(uint16_t bandwidth)
{
    assert(bandwidth == 188);
    ++setter_calls; return setter_error == 3 ? -1 : 0;
}
static void PIOS_HAL_CriticalError(uint32_t led, int code)
{
    assert(led == PIOS_LED_ALARM && code == PIOS_HAL_PANIC_IMU);
    panic = code;
    longjmp(failed, 1);
}
#include "omnibus_startup_production.inc"

static void boot(void)
{
    panic = allocation_calls = config_calls = setter_calls = 0;
    if (setjmp(failed) == 0) omnibus_imu_startup((pios_spi_t)1);
    /* Every fresh board start must allocate a fresh, initialized handle. */
    assert(allocation_calls == 1);
}
int main(void)
{
    const enum pios_mpu_type supported[] = {PIOS_MPU60X0, PIOS_MPU6500, PIOS_ICM20608G};
    for (unsigned i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        detected_type = supported[i];
        boot(); assert(!panic && config_calls == 1 && setter_calls == 3);
    }
    allocation_error = true; boot(); assert(panic == 2 && config_calls == 0);
    allocation_error = false;
    probe_error = -PIOS_MPU_ERROR_READFAILED; boot(); assert(panic == 2 && config_calls == 0);
    probe_error = 0;
    config_error = -PIOS_MPU_ERROR_NOIRQ; boot(); assert(panic == 2 && setter_calls == 0);
    config_error = 0;
    detected_type = PIOS_MPU9250; boot(); assert(panic == 2 && setter_calls == 0);
    detected_type = PIOS_MPU60X0;
    for (int i = 1; i <= 3; ++i) {
        setter_error = i; boot(); assert(panic == 2 && setter_calls == i);
    }
    setter_error = 0; boot(); assert(!panic && setter_calls == 3);
    puts("PASS: actual Omnibus board IMU caller and MPU initializer; null handle, supported chips, allocation/probe/IRQ/configuration failures");
    return 0;
}
