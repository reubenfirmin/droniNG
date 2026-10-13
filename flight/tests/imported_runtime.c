/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "pios_video_sync_guard.h"
#define PIOS_SERVO_DISABLE_DSHOT
#include "pios_servo_rate.h"
#include "imported_profile.h"

/* Hardware substitutes; functions under test are extracted unchanged from
 * production pios_spi.c and pios_bmi160.c by run_radix.py. */
#define PIOS_INCLUDE_IMPORTED
#define PIOS_Assert assert
#define SPI_I2S_FLAG_TXE 2
#define SPI_I2S_FLAG_RXNE 1
#define SPI_I2S_FLAG_BSY 128
struct registers { volatile uint32_t SR; uint8_t DR; } registers;
struct spi_config { struct registers *regs; } spi_config = { &registers };
struct spi_device { struct spi_config *cfg; } spi_device = { &spi_config };
typedef struct spi_device *pios_spi_t;
static uint32_t elapsed;
static int stalled, sent;
static uint32_t PIOS_DELAY_GetRaw(void) { return elapsed; }
static uint32_t PIOS_DELAY_DiffuS(uint32_t start) { return ++elapsed - start; }
static bool PIOS_SPI_validate(pios_spi_t device) { return device == &spi_device; }
static uint8_t SPI_ReceiveData8(struct registers *regs) { regs->SR &= ~SPI_I2S_FLAG_RXNE; return regs->DR; }
static void SPI_SendData8(struct registers *regs, uint8_t value) {
    ++sent; regs->DR = value ^ 0x55;
    if (stalled != 2) regs->SR |= SPI_I2S_FLAG_RXNE;
    if (stalled == 3) regs->SR |= SPI_I2S_FLAG_BSY;
}
#include "pios_bmi160.h"
struct pios_sensor_accel_data { float x, y, z, temperature; };
struct pios_sensor_gyro_data { float x, y, z, temperature; };
struct bmi160_dev {
    pios_spi_t spi_id; const struct pios_bmi160_cfg *cfg;
    struct pios_sensor_accel_data *accel_data; struct pios_sensor_gyro_data *gyro_data;
    float accel_scale, gyro_scale; uint8_t sensor_ready, temp_interleave_cnt;
};
#define BMI160_REG_GYR_DATA_X_LSB 0x0c
#define BMI160_REG_TEMPERATURE_0 0x20
#define SENSOR_ACCEL 1
static int claim_failure, transfer_failure, releases;
static int16_t sensor_values[6];
static int PIOS_BMI160_ClaimBus(void) { return claim_failure ? -1 : 0; }
static int PIOS_BMI160_ReleaseBus(void) { ++releases; return 0; }
static int PIOS_SPI_TransferBlock(pios_spi_t id, const uint8_t *tx, uint8_t *rx, int length) {
    (void)id; (void)tx;
    if (transfer_failure) return -1;
    memset(rx, 0, length);
    for (int i = 0; i < 6 && 2 * i + 2 < length; ++i) {
        rx[2 * i + 1] = sensor_values[i] & 255;
        rx[2 * i + 2] = (uint16_t)sensor_values[i] >> 8;
    }
    return 0;
}
#define FLIGHTBATTERYSETTINGS_SENSORCALIBRATIONFACTOR_VOLTAGE 0
#define FLIGHTBATTERYSETTINGS_SENSORCALIBRATIONFACTOR_CURRENT 1
typedef struct { float SensorCalibrationFactor[2]; } FlightBatterySettingsData;
static FlightBatterySettingsData battery, saved_battery;
static int battery_saved, battery_writes;
static void FlightBatterySettingsInitialize(void) {
    battery.SensorCalibrationFactor[0] = 90.90909f;
    battery.SensorCalibrationFactor[1] = 36.6f;
}
static uintptr_t FlightBatterySettingsHandle(void) { return 1; }
static int UAVObjLoad(uintptr_t handle, int instance) {
    assert(handle == 1 && instance == 0);
    if (!battery_saved) return -1;
    battery = saved_battery; return 0;
}
static void FlightBatterySettingsGet(FlightBatterySettingsData *data) { *data = battery; }
static void FlightBatterySettingsSet(const FlightBatterySettingsData *data) { battery = *data; ++battery_writes; }
#include "imported_production.inc"

static void test_spi(void) {
    uint8_t tx[] = {0, 1, 255}, rx[3];
    registers.SR = SPI_I2S_FLAG_TXE; elapsed = sent = stalled = 0;
    assert(SPI_PIO_TransferBlock(&spi_device, tx, rx, 3) == 0);
    assert(sent == 3 && rx[0] == 0x55 && rx[1] == 0x54 && rx[2] == 0xaa);
    for (int failure = 1; failure <= 4; ++failure) {
        stalled = failure; elapsed = sent = 0;
        registers.SR = failure == 1 ? 0 : SPI_I2S_FLAG_TXE;
        if (failure == 4) registers.SR |= SPI_I2S_FLAG_BSY;
        assert(SPI_PIO_TransferBlock(&spi_device, tx, rx, 1) == -1);
        assert(elapsed >= 1000 && elapsed <= 1100);
        assert(sent == (failure == 1 || failure == 4 ? 0 : 1));
    }
    stalled = 0; registers.SR = SPI_I2S_FLAG_TXE | SPI_I2S_FLAG_RXNE; elapsed = 0;
    assert(SPI_PIO_TransferBlock(&spi_device, tx, rx, 3) == 0);
    puts("PASS: actual SPI TX/RX/busy deadlines and recovery after an aborted transfer");
}

static void test_axes(void) {
    const struct pios_bmi160_cfg cfg = { .orientation = PIOS_BMI160_TOP_0DEG, .temperature_interleaving = 50 };
    struct pios_sensor_accel_data accel = {0}; struct pios_sensor_gyro_data gyro = {0};
    struct bmi160_dev dev = { .cfg = &cfg, .accel_data = &accel, .gyro_data = &gyro,
        .accel_scale = 9.81f / 4096, .gyro_scale = 2000.f / 32768, .temp_interleave_cnt = 1 };
    // Sensor x forward, y left, z up; expected aircraft frame is forward/right/down.
    const float expected[3][3] = {{1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
    for (int axis = 0; axis < 3; ++axis) for (int sign = -1; sign <= 1; sign += 2) {
        memset(sensor_values, 0, sizeof(sensor_values));
        sensor_values[axis] = sign * 16384; sensor_values[axis + 3] = sign * 4096;
        assert(PIOS_BMI160_parse_data(&dev) == 0);
        const float a[] = {accel.x, accel.y, accel.z}, g[] = {gyro.x, gyro.y, gyro.z};
        for (int i = 0; i < 3; ++i) {
            assert(fabsf(a[i] - expected[axis][i] * sign * 9.81f) < 0.001f);
            assert(fabsf(g[i] - expected[axis][i] * sign * 1000) < 0.001f);
        }
    }
    int before = releases; claim_failure = 1;
    assert(PIOS_BMI160_parse_data(&dev) == -2 && releases == before);
    claim_failure = 0; transfer_failure = 1;
    assert(PIOS_BMI160_parse_data(&dev) == -2 && releases == before + 1);
    transfer_failure = 0; assert(PIOS_BMI160_parse_data(&dev) == 0);
    puts("PASS: actual BMI160 signed axes, physical scales and acquisition failures");
}

static void test_sync(void) {
    struct pios_video_sync_guard guard = {0};
    assert(PIOS_Video_SyncAccept(&guard, 20000));
    for (int i = 1; i <= 10; ++i) assert(!PIOS_Video_SyncAccept(&guard, 20000 + i));
    assert(guard.masked); assert(!PIOS_Video_SyncRecover(&guard, 420009));
    assert(PIOS_Video_SyncRecover(&guard, 420010));
    assert(PIOS_Video_SyncAccept(&guard, 420010));
    assert(PIOS_Video_SyncAccept(&guard, 436677));
    assert(PIOS_Video_SyncAccept(&guard, 456677));
    guard.last_field = UINT32_MAX - 5000;
    assert(PIOS_Video_SyncAccept(&guard, 15000));
    for (int i = 1; i <= 10; ++i) assert(!PIOS_Video_SyncAccept(&guard, 15000 + i));
    assert(guard.masked); guard.masked_at = UINT32_MAX - 200000;
    assert(!PIOS_Video_SyncRecover(&guard, 199998));
    assert(PIOS_Video_SyncRecover(&guard, 200000));
    puts("PASS: video sync storm masking, recovery, PAL/NTSC intervals and timer wrap");
}
static void test_outputs(void) {
    uint16_t rates[] = {50, 400, SHOT_ONESHOT, 50, 50, 50};
    assert(PIOS_Servo_OutputRatesSupported(rates, 6));
    for (int bank = 0; bank < 6; ++bank) {
        const uint16_t previous = rates[bank];
        for (uint16_t mode = SHOT_DSHOT300; mode <= SHOT_DSHOT1200; ++mode) {
            rates[bank] = mode;
            assert(!PIOS_Servo_OutputRatesSupported(rates, 6));
        }
        rates[bank] = previous;
        assert(PIOS_Servo_OutputRatesSupported(rates, 6));
    }
    puts("PASS: PWM/OneShot accepted, DShot rejected on every bank, corrected settings recover");
}
static void test_battery(void) {
    battery_saved = battery_writes = 0;
    battery_settings_init();
    assert(battery_writes == 1);
    assert(fabsf(IMPORTED_ADC_VREF - 3.245f) < 0.00001f);
    assert(fabsf(battery.SensorCalibrationFactor[0] - 83.333333f) < 0.0001f);
    assert(fabsf(battery.SensorCalibrationFactor[1] - 20.f) < 0.0001f);
    for (int generic = 0; generic < 2; ++generic) {
        saved_battery.SensorCalibrationFactor[0] = generic ? 90.90909f : 85.f;
        saved_battery.SensorCalibrationFactor[1] = generic ? 36.6f : 17.f;
        battery_saved = 1; battery_writes = 0;
        battery_settings_init();
        assert(battery_writes == 0);
        assert(memcmp(&battery, &saved_battery, sizeof(battery)) == 0);
    }
    puts("PASS: manufacturer ADC reference/calibration, saved and custom calibration preserved");
}
int main(void) { test_spi(); test_axes(); test_sync(); test_outputs(); test_battery(); return 0; }
