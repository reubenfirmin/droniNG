/* SPDX-License-Identifier: GPL-3.0-or-later; simulated I2C, no controller access. */
#include "spracingf3_mock.h"
#include <pios_i2c.h>
#include <pios_mpu_priv.h>
#include <physical_constants.h>
#include "accgyro_mpu6050.h"
#include "spracingf3_profile.h"

struct pios_sensor_gyro_data { float x, y, z, temperature; };
struct pios_sensor_accel_data { float x, y, z, temperature; };
struct pios_mpu_dev {
    const struct pios_mpu_cfg *cfg;
    enum pios_mpu_type mpu_type;
    enum { PIOS_MPU_COM_I2C, PIOS_MPU_COM_SPI } com_driver_type;
    pios_i2c_t i2c_driver_id;
    uint32_t com_slave_addr, magic;
    enum pios_mpu_gyro_range gyro_range;
    enum pios_mpu_accel_range accel_range;
    struct pios_sensor_gyro_data gyro_data;
    struct pios_sensor_accel_data accel_data;
    uint8_t sensor_ready;
    bool half_resolution_accel;
};
#define PIOS_MPU_DEV_MAGIC 0xa9dd94b4
#define SENSOR_ACCEL 1
#define PIOS_SENSOR_ACCEL 0
#define PIOS_SENSOR_GYRO 1
static const struct pios_mpu_cfg config = {
    .default_samplerate = 1000, .orientation = SPRACINGF3_IMU_ORIENTATION
};
static struct pios_mpu_dev device = {
    .cfg = &config, .i2c_driver_id = (pios_i2c_t)1, .magic = PIOS_MPU_DEV_MAGIC,
    .mpu_type = PIOS_MPU60X0, .com_driver_type = PIOS_MPU_COM_I2C,
};
static struct pios_mpu_dev *mpu_dev = &device;
static uint8_t registers[128], present_address = 0x68;
static int failed_register = -1, failed_write = -1, gyro_limit, rates[2];
static bool transfer_failure, drop_writes, upstream_failed;
static int16_t samples[7] = {4096, -2048, 1024, 340, 164, -328, 492};
static int init_calls, shared_read_calls, elapsed_ms;
static void PIOS_SENSORS_SetMaxGyro(int maximum) { gyro_limit = maximum; }
static void PIOS_SENSORS_SetSampleRate(int sensor, int hz) { rates[sensor] = hz; }

int32_t PIOS_I2C_Transfer(pios_i2c_t id, const struct pios_i2c_txn txns[], uint32_t count)
{
    assert(id == (pios_i2c_t)1 && (count == 1 || count == 2));
    assert(txns[0].rw == PIOS_I2C_TXN_WRITE && txns[0].len == (count == 1 ? 2 : 1));
    const uint8_t reg = txns[0].buf[0];
    if (transfer_failure || reg == failed_register || txns[0].addr != present_address)
        return -1;
    if (count == 1) {
        if (reg == failed_write) return -1;
        if (!drop_writes) registers[reg] = txns[0].buf[1];
    } else {
        assert(txns[1].rw == PIOS_I2C_TXN_READ && txns[1].addr == txns[0].addr);
        if (txns[1].len == 14) {
            assert(reg == 0x3b);
            for (int i = 0; i < 7; ++i) {
                txns[1].buf[2 * i] = (uint16_t)samples[i] >> 8;
                txns[1].buf[2 * i + 1] = samples[i] & 255;
            }
        } else {
            assert(txns[1].len == 1);
            txns[1].buf[0] = registers[reg];
        }
    }
    return 0;
}

static bool upstream_write(uint8_t reg, uint8_t value)
{
    uint8_t data[] = {reg, value};
    const struct pios_i2c_txn txn = {
        .addr = present_address, .rw = PIOS_I2C_TXN_WRITE, .len = 2, .buf = data
    };
    bool success = PIOS_I2C_Transfer((pios_i2c_t)1, &txn, 1) == 0;
    upstream_failed |= !success;
    return success;
}
void delay(uint32_t milliseconds) { elapsed_ms += milliseconds; }
void mpuGyroInit(gyroDev_t *gyro) { (void)gyro; ++init_calls; }
bool mpuGyroRead(gyroDev_t *gyro) { (void)gyro; ++shared_read_calls; return false; }
bool mpuAccRead(accDev_t *acc) { (void)acc; ++shared_read_calls; return false; }
bool mpuCheckDataReady(gyroDev_t *gyro) { (void)gyro; return false; }
uint8_t gyroMPU6xxxGetDividerDrops(const gyroDev_t *gyro) { (void)gyro; return 7; }

static int32_t PIOS_MPU_ReadReg(uint8_t reg);
static int32_t PIOS_MPU_WriteReg(uint8_t reg, uint8_t value);
static int32_t PIOS_MPU_WriteRegVerify(uint8_t reg, uint8_t value);
#include "spracingf3_production.inc"

static void close_to(float actual, float expected) { assert(fabsf(actual - expected) < 0.001f); }

static void upstream_checks(void)
{
    gyroDev_t gyro = {.mpuDetectionResult = {.sensor = MPU_60x0},
        .mpuConfiguration = {.write = upstream_write}, .lpf = 1};
    accDev_t acc = {.mpuDetectionResult = {.sensor = MPU_60x0, .resolution = MPU_FULL_RESOLUTION}};
    assert(mpu6050GyroDetect(&gyro) && mpu6050AccDetect(&acc));
    assert(gyro.read == mpuGyroRead && acc.read == mpuAccRead);
    assert(gyro.intStatus == mpuCheckDataReady);
    gyro.init(&gyro); acc.init(&acc);
    assert(init_calls == 1 && shared_read_calls == 0 && elapsed_ms == 115 && !upstream_failed);
    assert(registers[0x6b] == 3 && registers[0x19] == 7 && registers[0x1a] == 1);
    assert(registers[0x1b] == 0x18 && registers[0x1c] == 0x18);
    assert(registers[0x37] == 2 && registers[0x38] == 1);
    close_to(gyro.scale, 1.0f / 16.4f);
    assert(acc.acc_1G == 2048 && acc.revisionCode == 'n'); // Actual full-resolution 16g setup.
    acc.mpuDetectionResult.resolution = MPU_HALF_RESOLUTION;
    assert(mpu6050AccDetect(&acc)); acc.init(&acc);
    assert(acc.acc_1G == 1024 && acc.revisionCode == 'o');
    gyro.mpuDetectionResult.sensor = MPU_NONE; acc.mpuDetectionResult.sensor = MPU_NONE;
    assert(!mpu6050GyroDetect(&gyro) && !mpu6050AccDetect(&acc));
    gyro.mpuDetectionResult.sensor = MPU_60x0;
    failed_write = 0x1b; upstream_failed = false; gyro.init(&gyro);
    assert(upstream_failed); // Upstream's void initializer ignores write failures; bridge must latch them.
    failed_write = -1;
    puts("PASS: untouched Betaflight MPU6050 source; callbacks, registers, rates, scales, full/half revisions and latched I2C errors");
}

static void native_checks(void)
{
    device.com_slave_addr = 0x68;
    for (int revision = 0; revision < 8; ++revision) {
        for (int i = 0; i < 3; ++i) registers[0x07 + 2 * i] = (revision >> i) & 1;
        registers[0x0c] = 0;
        int result = PIOS_MPU_Check6050Revision();
        assert((result == 0) == (revision == 1 || revision == 2 || revision == 3 || revision == 7));
        if (!result) {
            assert(device.half_resolution_accel == (revision == 1));
            assert(PIOS_MPU_SetAccelRange(PIOS_MPU_SCALE_8G) == 0);
            close_to(PIOS_MPU_GetAccelScale(), GRAVITY / (revision == 1 ? 2048 : 4096));
        }
    }
    registers[0x07] = registers[0x09] = registers[0x0b] = 0;
    for (int product = 1; product < 16; ++product) {
        registers[0x0c] = product;
        assert(PIOS_MPU_Check6050Revision() == 0);
        assert(device.half_resolution_accel == (product == 4));
    }
    for (int reg = 0x07; reg <= 0x0c; ++reg) {
        if (reg == 0x08 || reg == 0x0a) continue;
        failed_register = reg;
        assert(PIOS_MPU_Check6050Revision() == -PIOS_MPU_ERROR_READFAILED);
    }
    failed_register = -1; registers[0x0c] = 5;
    assert(PIOS_MPU_Check6050Revision() == 0 && !device.half_resolution_accel);
    enum pios_mpu_type detected = PIOS_MPU_NUM;
    for (int address = 0x68; address <= 0x69; ++address) {
        present_address = address; registers[0x75] = 0x68;
        assert(PIOS_MPU_I2C_Probe(&detected) == 0 && detected == PIOS_MPU60X0);
        assert(device.com_slave_addr == (uint32_t)address);
    }
    present_address = 0; assert(PIOS_MPU_I2C_Probe(&detected) < 0);
    present_address = 0x68; registers[0x75] = 0;
    assert(PIOS_MPU_I2C_Probe(&detected) < 0);
    registers[0x75] = 0x68; assert(PIOS_MPU_I2C_Probe(&detected) == 0);
    transfer_failure = true; assert(PIOS_MPU_I2C_Probe(&detected) < 0); transfer_failure = false;
    assert(PIOS_MPU_I2C_Probe(&detected) == 0);
    assert(PIOS_MPU_Validate(&device) == 0 && PIOS_MPU_Validate(NULL) < 0);
    const int checked[] = {0x1a, 0x19, 0x1b, 0x1c, 0x37, 0x38};
    for (unsigned i = 0; i < NELEMENTS(checked); ++i) {
        failed_write = checked[i]; assert(PIOS_MPU_Config(&config) < 0);
        failed_write = -1; failed_register = checked[i]; assert(PIOS_MPU_Config(&config) < 0);
        failed_register = -1;
    }
    memset(registers, 0xa5, sizeof(registers)); drop_writes = true;
    assert(PIOS_MPU_Config(&config) < 0); drop_writes = false;
    assert(PIOS_MPU_Config(&config) == 0 && rates[0] == 1000 && rates[1] == 1000);
    assert(PIOS_MPU_SetGyroRange(PIOS_MPU_SCALE_2000_DEG) == 0 && gyro_limit == 2000);
    assert(PIOS_MPU_SetAccelRange(PIOS_MPU_SCALE_8G) == 0);
    assert(registers[0x19] == 0 && registers[0x1a] == 1 && registers[0x1b] == 0x18 && registers[0x1c] == 0x10);
    assert(PIOS_MPU_parse_data(&device) == 0);
    // Upstream CW270 is (-rawY, rawX, rawZ); change Y/Z signs for native forward/right/down.
    close_to(device.accel_data.x, 0.5f * GRAVITY);
    close_to(device.accel_data.y, -GRAVITY); close_to(device.accel_data.z, -0.25f * GRAVITY);
    close_to(device.gyro_data.x, 20); close_to(device.gyro_data.y, -10); close_to(device.gyro_data.z, -30);
    close_to(device.gyro_data.temperature, 35 + 852.0f / 340);
    assert(device.sensor_ready == SENSOR_ACCEL);
    struct pios_sensor_gyro_data before_gyro = device.gyro_data;
    struct pios_sensor_accel_data before_accel = device.accel_data;
    device.sensor_ready = 0; transfer_failure = true;
    assert(PIOS_MPU_parse_data(&device) < 0 && device.sensor_ready == 0);
    assert(!memcmp(&before_gyro, &device.gyro_data, sizeof(before_gyro)));
    assert(!memcmp(&before_accel, &device.accel_data, sizeof(before_accel)));
    transfer_failure = false; samples[4] = -164;
    assert(PIOS_MPU_parse_data(&device) == 0); close_to(device.gyro_data.y, 10);
    assert(PIOS_MPU_SetSampleRate(0) < 0);
    puts("PASS: unchanged native MPU I2C functions; both addresses, detection, configuration, CW270 mapping, coherent signed samples and failure recovery");
}

int main(void)
{
    uint16_t prom[8] = {0, 40127, 36924, 23317, 23282, 33464, 28312, 0};
    assert(PIOS_MS5611_ValidatePROM(prom));
    for (unsigned i = 1; i <= 6; ++i) {
        for (unsigned bit = 0; bit < 16; ++bit) {
            prom[i] ^= 1u << bit;
            assert(!PIOS_MS5611_ValidatePROM(prom));
            prom[i] ^= 1u << bit;
        }
    }
    prom[7] = 1; assert(!PIOS_MS5611_ValidatePROM(prom));
    memset(prom, 0, sizeof(prom)); assert(!PIOS_MS5611_ValidatePROM(prom));
    memset(prom, 0xff, sizeof(prom)); assert(!PIOS_MS5611_ValidatePROM(prom));
    puts("PASS: MS5611 PROM CRC fixture, every coefficient bit, wrong checksum and absent-device patterns");
    upstream_checks();
    native_checks();
    return 0;
}
