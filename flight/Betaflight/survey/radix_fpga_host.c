/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unmodified upstream FPGA logic -> PiOS SPI bridge -> simulated hardware.
 * This exercises the adapter boundary; it does not implement a flight target.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radix_mock.h"
#include "fpga_drv.h"

static struct {
    uint8_t regs[16], bulk[3072];
    unsigned bulk_len, bulk_reg, elapsed_ms, transfers, config_pulses;
    unsigned reset_pulses, mco_calls, claim_calls;
    bool locked, selected, error, fail_bus, fail_claim, fail_transfer, fail_clock;
    bool cdone, pins[64];
    uint32_t hz;
} hw;

static pios_spi_t handle(void) { return (pios_spi_t)&hw; }

/* Implement the actual PiOS SPI declarations, checking transaction ordering. */
int32_t PIOS_SPI_ClaimBus(pios_spi_t spi)
{
    assert(spi == handle() && !hw.locked);
    ++hw.claim_calls;
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
int32_t PIOS_SPI_SetClockSpeed(pios_spi_t spi, uint32_t hz)
{
    assert(spi == handle() && hw.locked && !hw.selected);
    if (hw.fail_clock) return -1;
    hw.hz = hz;
    return (int32_t)hz;
}
int32_t PIOS_SPI_RC_PinSet(pios_spi_t spi, uint32_t slave, bool high)
{
    assert(spi == handle() && slave == 2 && hw.locked);
    hw.selected = !high;
    return 0;
}
int32_t PIOS_SPI_TransferBlock(pios_spi_t spi, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    assert(spi == handle() && hw.locked && hw.selected && tx && len >= 2);
    ++hw.transfers;
    if (hw.fail_transfer) return -1;
    uint8_t reg = tx[0] & 0x7f;
    assert(reg < 16);
    if (tx[0] & 0x80) {
        assert(rx && len == 2);
        rx[1] = hw.regs[reg];
    } else if (reg == 9 || reg == 15) {
        assert(len - 1u <= sizeof(hw.bulk));
        memcpy(hw.bulk, tx + 1, len - 1);
        hw.bulk_len = len - 1;
        hw.bulk_reg = reg;
    } else {
        assert(len == 2 && reg != 0);
        hw.regs[reg] = tx[1];
    }
    return 0;
}

/* Prototype service bridge. Failure latching is necessary for void writes. */
static bool transfer(const extDevice_t *dev, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    if (PIOS_SPI_ClaimBus(dev->spi) < 0) {
        hw.error = true;
        return false;
    }
    bool ok = PIOS_SPI_SetClockSpeed(dev->spi, dev->hz) > 0;
    if (ok) ok = PIOS_SPI_RC_PinSet(dev->spi, 2, false) >= 0;
    if (ok) ok = PIOS_SPI_TransferBlock(dev->spi, tx, rx, len) >= 0;
    ok = PIOS_SPI_RC_PinSet(dev->spi, 2, true) >= 0 && ok;
    ok = PIOS_SPI_ReleaseBus(dev->spi) >= 0 && ok;
    hw.error |= !ok;
    return ok;
}
void spiWriteReg(const extDevice_t *dev, uint8_t reg, uint8_t data)
{
    const uint8_t tx[] = {reg, data};
    transfer(dev, tx, NULL, sizeof(tx));
}
void spiWriteRegBuf(const extDevice_t *dev, uint8_t reg, const uint8_t *data, unsigned len)
{
    uint8_t tx[3073];
    assert(len <= sizeof(tx) - 1);
    tx[0] = reg;
    memcpy(tx + 1, data, len);
    transfer(dev, tx, NULL, (uint16_t)(len + 1));
}
int spiDeviceByInstance(int instance) { assert(instance == SPI3); return instance; }
bool spiSetBusInstance(extDevice_t *dev, int bus)
{
    assert(bus == SPI3);
    dev->spi = handle();
    return !hw.fail_bus;
}
void spiSetClkDivisor(extDevice_t *dev, uint16_t divisor)
{
    assert(divisor == 16);
    dev->hz = 45000000u / divisor;
}

IO_t IOGetByTag(uint16_t tag) { assert(tag < 64); return tag; }
void IOInit(IO_t pin, unsigned owner, unsigned index)
{
    assert(pin < 64 && owner == OWNER_OSD && index == 0);
}
void IOConfigGPIO(IO_t pin, unsigned config) { assert(pin < 64 && config); }
void IOConfigGPIOAF(IO_t pin, unsigned config, unsigned af)
{
    assert(pin == PA8 && config == IOCFG_AF_PP && af == GPIO_AF_MCO);
}
void IOHi(IO_t pin)
{
    assert(pin < 64);
    if (pin == PC4) ++hw.reset_pulses;
    hw.pins[pin] = true;
}
void IOLo(IO_t pin)
{
    assert(pin < 64);
    if (pin == PB1) ++hw.config_pulses;
    hw.pins[pin] = false;
}
bool IORead(IO_t pin) { assert(pin == PB12); return hw.cdone; }
void delay(unsigned ms) { hw.elapsed_ms += ms; }
void RCC_MCO1Config(unsigned source, unsigned divider)
{
    assert(source == RCC_MCO1Source_HSE && divider == RCC_MCO1Div_1);
    ++hw.mco_calls;
}
void RCC_MCO1Cmd(unsigned enable) { assert(enable == ENABLE); }

static void reset(void)
{
    memset(&hw, 0, sizeof(hw));
    hw.cdone = true;
    hw.regs[0] = 0x42; /* Deliberately nonzero to expose the upstream getter. */
}

extern extDevice_t fpga_spi_dev;

/* A production initializer needs both error propagation and real identity IO. */
static int checked_init(bool load_config, uint8_t *revision)
{
    hw.error = false;
    int result = BRAINFPVFPGA_Init(load_config);
    if (result) return result;
    if (hw.error) return -3;
    const uint8_t tx[] = {0x80, 0};
    uint8_t rx[2] = {0};
    if (!transfer(&fpga_spi_dev, tx, rx, sizeof(tx))) return -3;
    if (rx[1] == 0 || rx[1] == 0xff) return -4;
    *revision = rx[1];
    return 0;
}

int main(void)
{
    reset();
    assert(BRAINFPVFPGA_Init(true) == 0 && !hw.error);
    assert(hw.config_pulses == 1 && hw.reset_pulses == 1 && hw.mco_calls == 1);
    assert(hw.pins[PC15] && !hw.pins[PC4] && hw.hz == 2812500);
    assert(hw.regs[1] == 0 && hw.regs[2] == 0);
    assert(hw.regs[3] == 35 && hw.regs[4] == 110 && hw.regs[5] == 30);
    assert(hw.regs[6] == 8 && hw.regs[7] == 16 && hw.regs[8] == 0);

    assert(BRAINFPVFPGA_SerialRxInvert(true) == 0 && hw.regs[1] == 1);
    assert(BRAINFPVFPGA_MPTxPinMode(true, true) == 0 && hw.regs[1] == 7);
    assert(BRAINFPVFPGA_MPTxPinPullUpDown(true, true) == 0 && hw.regs[1] == 31);
    assert(BRAINFPVFPGA_SetBuzzerType(BRAINFPVFPGA_BUZZER_AC) == 0 && hw.regs[1] == 95);
    assert(BRAINFPVFPGA_Buzzer(true) == 0 && hw.regs[2] == 1);
    assert(BRAINFPVFPGA_AlarmLED(true) == 0 && hw.regs[2] == 5);
    assert(BRAINFPVFPGA_BuzzerToggle() == 0 && hw.regs[2] == 4);
    assert(BRAINFPVFPGA_SerialRxInvert(false) == 0 && hw.regs[1] == 94);
    BRAINFPVFPGA_SetBwLevels(20, 120);
    assert(hw.regs[3] == 20 && hw.regs[4] == 120);
    assert(BRAINFPVFPGA_SetSyncThreshold(25) == 0 && hw.regs[5] == 25);
    BRAINFPVFPGA_SetXScale(0x13);
    BRAINFPVFPGA_SetXOffset(127);
    assert(hw.regs[6] == 0x3f);
    BRAINFPVFPGA_SetXOffset(-128);
    assert(hw.regs[6] == 0x30);
    BRAINFPVFPGA_Set3DConfig(true, 255);
    assert(hw.regs[7] == 0x7f);
    uint8_t data[4096];
    memset(data, 0x5a, sizeof(data));
    assert(BRAINFPVFPGA_SetIRData(data, 17) == -1);
    assert(BRAINFPVFPGA_SetIRData(data, 16) == 0 && hw.bulk_reg == 9 && hw.bulk_len == 16);
    assert(BRAINFPVFPGA_SetLEDs(data, 2000) == 0 && hw.bulk_reg == 15 && hw.bulk_len == 3072);
    assert(!hw.locked && !hw.selected && !hw.error);
    puts("PASS: unchanged RADIX FPGA driver, PiOS SPI ordering, OSD and serial/buzzer controls");

    uint8_t revision = 0;
    /* Getter never reads HWREV in this source pin; a wrapper must read it. */
    assert(BRAINFPVFPGA_GetHWRevision() == 0 && hw.regs[0] == 0x42);
    reset();
    assert(checked_init(false, &revision) == 0 && revision == 0x42 && !hw.config_pulses);
    reset();
    hw.fail_bus = true;
    assert(checked_init(true, &revision) == -1 && hw.transfers == 0);
    reset();
    hw.cdone = false;
    assert(checked_init(true, &revision) == -2 && hw.elapsed_ms == 101 && hw.transfers == 0);
    reset();
    hw.fail_claim = true;
    assert(checked_init(true, &revision) == -3 && !hw.locked && !hw.selected && hw.transfers == 0);
    reset();
    hw.fail_clock = true;
    assert(checked_init(true, &revision) == -3 && !hw.locked && !hw.selected && hw.transfers == 0);
    reset();
    hw.fail_transfer = true;
    assert(BRAINFPVFPGA_Init(true) == 0 && hw.error && !hw.locked && !hw.selected);
    assert(checked_init(true, &revision) == -3 && !hw.locked && !hw.selected);
    reset();
    assert(checked_init(true, &revision) == 0);
    hw.fail_transfer = true;
    unsigned transfers = hw.transfers;
    assert(BRAINFPVFPGA_SerialRxInvert(true) == 0 && hw.error && hw.regs[1] == 0);
    assert(hw.transfers == transfers + 1);
    hw.fail_transfer = false;
    assert(BRAINFPVFPGA_SerialRxInvert(true) == 0 && hw.regs[1] == 0);
    assert(hw.transfers == transfers + 1); /* Failed write was cached as success. */
    reset();
    hw.regs[0] = 0;
    assert(checked_init(true, &revision) == -4);
    reset();
    hw.regs[0] = 0xff;
    assert(checked_init(true, &revision) == -4);
    puts("PASS: missing FPGA, bus/clock/transfer failures, false init success, cached failed writes and missing revision read exposed");
    return 0;
}
