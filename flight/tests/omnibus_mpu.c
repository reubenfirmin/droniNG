/* SPDX-License-Identifier: GPL-3.0-or-later; unchanged production functions use host hardware substitutes. */
#include <pios.h>
#include <pios_mpu_priv.h>
#include <physical_constants.h>
#include <math.h>
#include <stdio.h>
#include "imported_profile.h"

struct pios_sensor_gyro_data { float x, y, z, temperature; };
struct pios_sensor_accel_data { float x, y, z, temperature; };
struct pios_mpu_dev {
    const struct pios_mpu_cfg *cfg;
    enum pios_mpu_type mpu_type;
    pios_spi_t spi_driver_id;
    uint32_t com_slave_addr, magic;
    enum { PIOS_MPU_COM_I2C, PIOS_MPU_COM_SPI } com_driver_type;
    enum pios_mpu_gyro_range gyro_range;
    enum pios_mpu_accel_range accel_range;
    struct pios_sensor_gyro_data gyro_data;
    struct pios_sensor_accel_data accel_data;
    uint8_t sensor_ready;
};
#define PIOS_MPU_DEV_MAGIC 0xa9dd94b4
#define PIOS_MPU_SPI_LOW_SPEED 300000
#define PIOS_MPU_SPI_HIGH_SPEED 20000000
#define SENSOR_ACCEL 1
#define PIOS_SENSOR_ACCEL 0
#define PIOS_SENSOR_GYRO 1
static struct pios_mpu_cfg config = {.default_samplerate = 1000, .orientation = IMPORTED_IMU_ORIENTATION};
static struct pios_mpu_dev device = {.cfg = &config, .spi_driver_id = (pios_spi_t)1, .com_slave_addr = 0,
    .magic = PIOS_MPU_DEV_MAGIC, .mpu_type = PIOS_MPU6500, .com_driver_type = PIOS_MPU_COM_SPI};
static struct pios_mpu_dev *mpu_dev = &device;
static uint8_t registers[128];
static bool claimed, selected, claim_failure, clock_failure, transfer_failure, drop_writes;
static int failed_register = -1, releases, gyro_limit, rates[2];
static int32_t speed;
static int16_t samples[7] = {4096, -2048, 1024, 0, 164, -328, 492};
static void PIOS_SENSORS_SetMaxGyro(int maximum) { gyro_limit = maximum; }
static void PIOS_SENSORS_SetSampleRate(int sensor, int hz) { rates[sensor] = hz; }
int32_t PIOS_SPI_ClaimBus(pios_spi_t id) {
    assert(id == (pios_spi_t)1 && !claimed && !selected);
    if (claim_failure) return -1;
    claimed = true; return 0;
}
int32_t PIOS_SPI_ReleaseBus(pios_spi_t id) {
    assert(id == (pios_spi_t)1 && claimed && !selected); claimed = false; ++releases; return 0;
}
int32_t PIOS_SPI_SetClockSpeed(pios_spi_t id, uint32_t requested) {
    assert(id == (pios_spi_t)1 && claimed && !selected);
    assert(requested == PIOS_MPU_SPI_LOW_SPEED || requested == PIOS_MPU_SPI_HIGH_SPEED);
    if (clock_failure) return 0;
    speed = requested; return speed;
}
int32_t PIOS_SPI_RC_PinSet(pios_spi_t id, uint32_t slave, bool high) {
    assert(id == (pios_spi_t)1 && slave == 0 && claimed && selected == high);
    assert(speed == PIOS_MPU_SPI_LOW_SPEED || speed == PIOS_MPU_SPI_HIGH_SPEED);
    selected = !high; return 0;
}
int32_t PIOS_SPI_TransferBlock(pios_spi_t id, const uint8_t *tx, uint8_t *rx, uint16_t size) {
    assert(id == (pios_spi_t)1 && claimed && selected);
    if (transfer_failure || (tx[0] & 0x7f) == failed_register) return -1;
    const int reg = tx[0] & 0x7f;
    if (size == 15) {
        assert(speed == PIOS_MPU_SPI_HIGH_SPEED && reg == PIOS_MPU_ACCEL_X_OUT_MSB && rx);
        rx[0] = 0;
        for (int i = 0; i < 7; ++i) { rx[1 + 2 * i] = (uint16_t)samples[i] >> 8; rx[2 + 2 * i] = samples[i] & 255; }
    } else {
        assert(size == 2 && speed == PIOS_MPU_SPI_LOW_SPEED);
        if (rx) { rx[0] = 0; rx[1] = registers[reg]; }
        else if (!drop_writes) registers[reg] = tx[1];
    }
    return 0;
}
static int32_t PIOS_MPU_ReadReg(uint8_t reg);
static int32_t PIOS_MPU_WriteReg(uint8_t reg, uint8_t value);
static int32_t PIOS_MPU_WriteRegVerify(uint8_t reg, uint8_t value);
#include "omnibus_production.inc"

static void close_to(float a, float b) { assert(fabsf(a - b) < 0.001f); }
static void test_axes_and_acquisition(void) {
    assert(PIOS_MPU_SetGyroRange(PIOS_MPU_SCALE_2000_DEG) == 0 && gyro_limit == 2000);
    assert(PIOS_MPU_SetAccelRange(PIOS_MPU_SCALE_8G) == 0);
    speed = 9000000; // A previous user left another clock rate on the bus.
    assert(PIOS_MPU_parse_data(&device) == 0 && speed == PIOS_MPU_SPI_HIGH_SPEED);
    // Betaflight CW270 maps (-rawY, rawX, rawZ) in its frame. Conversion to
    // droniNG's forward/right/down axes negates Y and Z.
    close_to(device.accel_data.x, 0.5f * GRAVITY); close_to(device.accel_data.y, -GRAVITY);
    close_to(device.accel_data.z, -0.25f * GRAVITY);
    close_to(device.gyro_data.x, 20); close_to(device.gyro_data.y, -10); close_to(device.gyro_data.z, -30);
    assert(device.sensor_ready == SENSOR_ACCEL); close_to(device.gyro_data.temperature, 21);
    for (int failure = 0; failure < 3; ++failure) {
        struct pios_sensor_gyro_data before = device.gyro_data;
        device.sensor_ready = 0;
        claim_failure = failure == 0; clock_failure = failure == 1; transfer_failure = failure == 2;
        int previous = releases;
        assert(PIOS_MPU_parse_data(&device) < 0);
        assert(!memcmp(&before, &device.gyro_data, sizeof(before)) && device.sensor_ready == 0);
        assert(!claimed && !selected && releases - previous == (failure != 0));
        claim_failure = clock_failure = transfer_failure = false;
    }
    assert(PIOS_MPU_parse_data(&device) == 0);
    puts("PASS: actual MPU acquisition; Omnibus CW270 axes, units, coherent samples and shared-bus/failure recovery");
}
int main(void) {
    const uint8_t ids[] = {0x68, 0x70, 0xaf};
    const enum pios_mpu_type types[] = {PIOS_MPU60X0, PIOS_MPU6500, PIOS_ICM20608G};
    for (unsigned i = 0; i < sizeof(ids); ++i) {
        registers[PIOS_MPU_WHOAMI] = ids[i]; enum pios_mpu_type found = PIOS_MPU_NUM;
        assert(PIOS_MPU_CheckWhoAmI(&found) == 0 && found == types[i]);
    }
    registers[PIOS_MPU_WHOAMI] = 0; enum pios_mpu_type found;
    assert(PIOS_MPU_CheckWhoAmI(&found) < 0);
    transfer_failure = true; assert(PIOS_MPU_CheckWhoAmI(&found) < 0); transfer_failure = false;
    const int checked[] = {PIOS_MPU_DLPF_CFG_REG, PIOS_MPU_ACCEL_CFG2_REG, PIOS_MPU_SMPLRT_DIV_REG,
        PIOS_MPU_GYRO_CFG_REG, PIOS_MPU_ACCEL_CFG_REG, PIOS_MPU_INT_CFG_REG, PIOS_MPU_INT_EN_REG};
    for (unsigned i = 0; i < sizeof(checked) / sizeof(checked[0]); ++i) {
        failed_register = checked[i]; assert(PIOS_MPU_Config(&config) < 0);
        assert(!claimed && !selected);
    }
    failed_register = -1; memset(registers, 0xa5, sizeof(registers)); drop_writes = true;
    assert(PIOS_MPU_Config(&config) < 0); drop_writes = false;
    assert(PIOS_MPU_Config(&config) == 0 && rates[0] == 1000 && rates[1] == 1000);
    assert(PIOS_MPU_SetSampleRate(0) < 0);
    enum pios_mpu_gyro_range before = device.gyro_range;
    transfer_failure = true; assert(PIOS_MPU_SetGyroRange(PIOS_MPU_SCALE_2000_DEG) < 0);
    assert(device.gyro_range == before); transfer_failure = false;
    puts("PASS: actual MPU detection/configuration; missing/unrecognized device, failed and discarded register writes rejected");
    test_axes_and_acquisition();
    return 0;
}
