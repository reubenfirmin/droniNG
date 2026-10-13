/* SPDX-License-Identifier: GPL-3.0-or-later
 * Actual upstream chip logic -> experimental PiOS API bridge -> fake hardware.
 * No Betaflight MPU acquisition/DMA/EXTI implementation is compiled here.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "survey_mock.h"
#include "drivers/accgyro/accgyro_spi_icm426xx.h"

struct surveyGyro gyro;
static gyroConfig_t config;
static struct gyroDeviceConfig_s device_config = {.clkIn = 15};
static struct {
    uint8_t regs[5][128], bank;
    bool locked, selected, fail_claim, fail_transfer, error, clock_ok;
    unsigned transfers, elapsed_ms, mpu_init_calls, clock_calls;
    uint32_t hz, desired_hz;
} hw;
static pios_spi_t handle(void) { return (pios_spi_t)&hw; }

/* Mock implementations use the real PiOS SPI declarations. */
int32_t PIOS_SPI_ClaimBus(pios_spi_t spi)
{
    assert(spi == handle() && !hw.locked);
    if (hw.fail_claim) return -1;
    hw.locked = true;
    return 0;
}
int32_t PIOS_SPI_ReleaseBus(pios_spi_t spi)
{
    assert(spi == handle() && hw.locked && !hw.selected);
    hw.locked = false;
    return 0;
}
int32_t PIOS_SPI_SetClockSpeed(pios_spi_t spi, uint32_t speed)
{
    assert(spi == handle() && hw.locked && !hw.selected);
    hw.hz = speed;
    return (int32_t)speed;
}
int32_t PIOS_SPI_RC_PinSet(pios_spi_t spi, uint32_t slave, bool high)
{
    assert(spi == handle() && slave == 0 && hw.locked);
    hw.selected = !high;
    return 0;
}
int32_t PIOS_SPI_TransferBlock(pios_spi_t spi, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    assert(spi == handle() && hw.locked && hw.selected && tx && len >= 2);
    ++hw.transfers;
    if (hw.fail_transfer) return -1;
    uint8_t reg = tx[0] & 0x7f;
    assert(reg + len - 1 <= 128);
    if (tx[0] & 0x80) {
        assert(rx);
        rx[0] = 0;
        memcpy(rx + 1, &hw.regs[hw.bank][reg], len - 1);
    } else {
        assert(len == 2);
        if (reg == 0x76) {
            assert(tx[1] < 5);
            hw.bank = tx[1];
        } else {
            hw.regs[hw.bank][reg] = tx[1];
        }
    }
    return 0;
}

/* Experimental bridge: all transactions own the bus, set rate, then toggle CS.
 * A latched failure is necessary because upstream writes return void.
 */
static bool transfer(const extDevice_t *dev, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (PIOS_SPI_ClaimBus(dev->spi) < 0) { hw.error = true; return false; }
    bool ok = PIOS_SPI_SetClockSpeed(dev->spi, hw.desired_hz) > 0;
    if (ok) ok = PIOS_SPI_RC_PinSet(dev->spi, dev->slave, false) >= 0;
    if (ok) ok = PIOS_SPI_TransferBlock(dev->spi, tx, rx, len) >= 0;
    ok = PIOS_SPI_RC_PinSet(dev->spi, dev->slave, true) >= 0 && ok;
    ok = PIOS_SPI_ReleaseBus(dev->spi) >= 0 && ok;
    hw.error |= !ok;
    return ok;
}
void spiWriteReg(const extDevice_t *dev, uint8_t reg, uint8_t data)
{
    const uint8_t tx[] = {reg, data};
    transfer(dev, tx, NULL, sizeof(tx));
}
uint8_t spiReadRegMsk(const extDevice_t *dev, uint8_t reg)
{
    const uint8_t tx[] = {reg | 0x80, 0};
    uint8_t rx[2] = {0};
    return transfer(dev, tx, rx, sizeof(tx)) ? rx[1] : 0;
}
uint16_t spiCalculateDivider(uint32_t hz)
{
    uint16_t divisor = 2;
    while (84000000u / divisor > hz && divisor < 256) divisor *= 2;
    return divisor;
}
void spiSetClkDivisor(const extDevice_t *dev, uint16_t divisor)
{
    hw.desired_hz = dev->bus_hz / divisor;
}
void delay(unsigned ms) { hw.elapsed_ms += ms; }
const gyroConfig_t *gyroConfig(void) { return &config; }
void mpuGyroInit(gyroDev_t *dev) { (void)dev; ++hw.mpu_init_calls; }

/* Replacement synchronous acquisition for the probe, not upstream MPU code. */
static bool read_axes(const extDevice_t *dev, uint8_t reg, int16_t raw[3])
{
    uint8_t tx[7] = {reg | 0x80}, rx[7] = {0};
    if (!transfer(dev, tx, rx, sizeof(tx))) return false;
    for (unsigned i = 0; i < 3; ++i) {
        uint16_t value = (uint16_t)rx[1 + 2*i] << 8 | rx[2 + 2*i];
        raw[i] = value < 32768 ? (int16_t)value : (int16_t)((int32_t)value - 65536);
    }
    return true;
}
bool mpuGyroReadSPI(gyroDev_t *dev) { return read_axes(&dev->dev, dev->gyroDataReg, dev->raw); }
bool mpuAccReadSPI(accDev_t *dev) { return read_axes(&dev->gyro->dev, dev->gyro->accDataReg, dev->raw); }

const struct gyroDeviceConfig_s *gyroDeviceConfig(unsigned i) { assert(i == 0); return &device_config; }
IO_t IOGetByTag(ioTag_t tag) { return tag; }
bool gyroClkInInit(ioTag_t tag, uint32_t hz, unsigned owner)
{
    assert(tag == 15 && hz == 32000 && owner == 1);
    ++hw.clock_calls;
    return hw.clock_ok;
}
static gyroDev_t *reset(uint8_t who)
{
    memset(&hw, 0, sizeof(hw));
    memset(&gyro, 0, sizeof(gyro));
    hw.regs[0][0x75] = who;
    hw.regs[0][0x64] = 0x10;
    hw.regs[0][0x4d] = 0xc0;
    hw.regs[1][0x7b] = 0x82;
    hw.desired_hz = 1000000;
    hw.clock_ok = true;
    config.gyro_hardware_lpf = GYRO_HARDWARE_LPF_NORMAL;
    gyroDev_t *dev = &gyro.gyroSensor[0].gyroDev;
    dev->dev = (extDevice_t){handle(), 0, 84000000};
    dev->mpuDetectionResult.sensor = ICM_42688P_SPI;
    dev->gyroRateKHz = GYRO_RATE_8_kHz;
    return dev;
}
static void expected_init(unsigned divider_drops, uint8_t odr, bool fallback)
{
    gyroDev_t *dev = reset(0x47);
    assert(icm426xxSpiDetect(&dev->dev) == ICM_42688P_SPI);
    assert(icm426xxSpiGyroDetect(dev));
    accDev_t acc = {.gyro = dev, .mpuDetectionResult = dev->mpuDetectionResult};
    assert(icm426xxSpiAccDetect(&acc));
    acc.initFn(&acc);
    assert(acc.acc_1G == 2048);
    dev->mpuDividerDrops = divider_drops;
    dev->initFn(dev);
    assert(hw.mpu_init_calls == 1 && !hw.error);
    assert(hw.hz == 21000000 && hw.hz <= 24000000);
    assert(hw.regs[0][0x4f] == odr && hw.regs[0][0x50] == odr);
    assert(hw.regs[0][0x4e] == 0x0f);
    assert(hw.regs[1][0x0c] == 6 && hw.regs[1][0x0d] == 36 && hw.regs[1][0x0e] == 0xa0);
    assert(hw.regs[2][0x03] == 12 && hw.regs[2][0x04] == 36 && hw.regs[2][0x05] == 0xa0);
    assert(hw.regs[0][0x14] == 0x03 && hw.regs[0][0x65] == 0x08);
    assert(hw.regs[0][0x64] == 0x60 && hw.regs[0][0x52] == 0xff);
#ifdef USE_GYRO_CLKIN
    /* Upstream's static clock state persists across fixture resets. */
    assert(hw.clock_calls <= 1 && hw.regs[1][0x7b] == 0x84 && hw.regs[0][0x4d] == 0x44);
#else
    assert(hw.clock_calls == 0 && hw.regs[0][0x4d] == 0x40);
#endif
    assert(hw.bank == 0 && !hw.locked && !hw.selected && hw.elapsed_ms >= 34);
    assert(!fallback || dev->gyroRateKHz == GYRO_RATE_1_kHz);
    const uint8_t samples[] = {0x08, 0x00, 0xf8, 0x00, 0x7f, 0xff};
    memcpy(&hw.regs[0][0x25], samples, sizeof(samples));
    memcpy(&hw.regs[0][0x1f], samples, sizeof(samples));
    assert(dev->readFn(dev) && acc.readFn(&acc));
    assert(dev->raw[0] == 2048 && dev->raw[1] == -2048 && dev->raw[2] == 32767);
    assert(acc.raw[0] == 2048 && acc.raw[1] == -2048);
    assert(fabsf(dev->raw[0] * dev->scale - 125.0f) < 0.001f);
    assert(fabsf(13248 * dev->tempScale + dev->tempZero - 125.0f) < 0.001f);
    /* A failed read must not publish a zero/stale frame as new data. */
    hw.fail_transfer = true;
    assert(!dev->readFn(dev) && hw.error && !hw.selected && !hw.locked);
    assert(dev->raw[0] == 2048);
}
int main(void)
{
#ifdef USE_GYRO_CLKIN
    /* Exercise failure before the upstream static clock state becomes enabled. */
    gyroDev_t *clock_dev = reset(0x47);
    hw.clock_ok = false;
    assert(icm426xxSpiDetect(&clock_dev->dev) == ICM_42688P_SPI);
    assert(hw.clock_calls == 1 && hw.regs[1][0x7b] == 0x82);
    assert(!(hw.regs[0][0x4d] & 4)); /* upstream falls back silently */
#endif
    expected_init(0, 3, false);  /* 8 kHz */
    expected_init(1, 4, false);  /* 4 kHz */
    expected_init(3, 5, false);  /* 2 kHz */
    expected_init(7, 6, false);  /* 1 kHz */
    expected_init(15, 6, true);  /* invalid decimation -> 1 kHz fallback */
    gyroDev_t *dev = reset(0);
    assert(icm426xxSpiDetect(&dev->dev) == MPU_NONE);
    dev->mpuDetectionResult.sensor = MPU_NONE;
    assert(!icm426xxSpiGyroDetect(dev));
    dev = reset(0x47);
    hw.fail_claim = true;
    assert(icm426xxSpiDetect(&dev->dev) == MPU_NONE && hw.error && hw.transfers == 0);
    dev = reset(0x47);
    hw.fail_transfer = true;
    assert(icm426xxSpiDetect(&dev->dev) == MPU_NONE && hw.error && !hw.locked && !hw.selected);
    dev = reset(0x47);
    assert(icm426xxSpiGyroDetect(dev));
    hw.fail_transfer = true;
    dev->initFn(dev);
    /* Upstream init returns void. A real PiOS wrapper must reject this state. */
    assert(hw.error);
#ifdef USE_GYRO_CLKIN
    puts("PASS: upstream ICM42688 + PiOS SPI mock, external-clock success and fallback");
#else
    puts("PASS: upstream ICM42688 + PiOS SPI mock, internal clock");
#endif
    return 0;
}
