/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise the actual shared native FPGA driver, with a simulated PiOS bus.
 */
#include <pios.h>
#include "fpga_drv.h"
#include <stdio.h>

static struct {
    uint8_t regs[16], bulk[3072], address;
    unsigned transfers, bulk_size, waited, clocks, reset_pulses;
    bool locked, selected, cdone, fail_claim, fail_clock, fail_transfer, drop_writes;
    unsigned fail_at;
    uint32_t hz;
} hw;
static void *allocations[32];
static unsigned allocated;
void *PIOS_malloc(size_t size) { assert(allocated < 32); return allocations[allocated++] = calloc(1, size); }
void GPIO_Init(void *port, GPIO_InitTypeDef *init) { assert(port && init); }
void GPIO_ResetBits(void *port, uint16_t pin) { assert(port); if (pin == 8) ++hw.reset_pulses; }
void GPIO_SetBits(void *port, uint16_t pin) { assert(port && pin); }
bool GPIO_ReadInputDataBit(void *port, uint16_t pin) { assert(port && pin == 4); return hw.cdone; }
void GPIO_PinAFConfig(void *port, unsigned pin, unsigned af) { assert(port); }
void RCC_MCO1Config(unsigned source, unsigned divider) { assert(source == 1 && divider == 1); ++hw.clocks; }
void RCC_MCO1Cmd(unsigned enabled) { assert(enabled == 1); }
void PIOS_DELAY_WaitmS(unsigned ms) { hw.waited += ms; }
static pios_spi_t bus(void) { return (pios_spi_t)&hw; }
int32_t PIOS_SPI_ClaimBus(pios_spi_t spi) {
    assert(spi == bus() && !hw.locked);
    if (hw.fail_claim) return -1;
    hw.locked = true; return 0;
}
int32_t PIOS_SPI_SetClockSpeed(pios_spi_t spi, uint32_t hz) {
    assert(spi == bus() && hw.locked && !hw.selected && hz == 2812500);
    if (hw.fail_clock) return -1;
    hw.hz = hz; return hz;
}
int32_t PIOS_SPI_RC_PinSet(pios_spi_t spi, uint32_t slave, bool high) {
    assert(spi == bus() && slave == 2 && hw.locked);
    if (!high) { assert(hw.hz == 2812500); hw.address = 255; }
    hw.selected = !high; return 0;
}
int32_t PIOS_SPI_ReleaseBus(pios_spi_t spi) {
    assert(spi == bus() && hw.locked && !hw.selected);
    hw.locked = false; return 0;
}
int32_t PIOS_SPI_TransferBlock(pios_spi_t spi, const uint8_t *tx, uint8_t *rx, uint16_t length) {
    assert(spi == bus() && hw.locked && hw.selected && tx && length);
    ++hw.transfers;
    if (hw.fail_transfer || hw.transfers == hw.fail_at) return -1;
    if (hw.address == 255) {
        hw.address = tx[0] & 0x7f; assert(hw.address < 16);
        if (tx[0] & 0x80) {
            assert(rx && length == 2); rx[1] = hw.regs[hw.address]; return 0;
        }
        if (length == 1) return 0;
        assert(length == 2 && hw.address != 0);
        if (!hw.drop_writes) hw.regs[hw.address] = tx[1];
    } else {
        assert(length <= sizeof(hw.bulk)); memcpy(hw.bulk, tx, length); hw.bulk_size = length;
    }
    return 0;
}
static const struct pios_re1fpga_cfg config = {
    .mco_pin = { (void*)1, {1} }, .rst_pin = { (void*)1, {2} },
    .cdone_pin = { (void*)1, {4} }, .cresetb_pin = { (void*)1, {8} },
    .spi_hz = 2812500, .sync_threshold = 30,
};
static void reset(void) { memset(&hw, 0, sizeof(hw)); hw.cdone = true; hw.regs[0] = 6; }
static void released(void) { assert(!hw.locked && !hw.selected); }
int main(void) {
    reset(); assert(!PIOS_RE1FPGA_Init(bus(), 2, &config, true)); released();
    assert(hw.clocks == 1 && hw.reset_pulses == 1 && hw.waited == 22);
    assert(PIOS_RE1FPGA_GetHWRevision() == 6 && hw.regs[3] == 35 && hw.regs[4] == 110 && hw.regs[5] == 30);
    hw.hz = 5625000; assert(!PIOS_RE1FPGA_SerialRxInvert(true)); assert(hw.regs[1] & 1);
    assert(!PIOS_RE1FPGA_MPTxPinMode(true, true)); assert(!PIOS_RE1FPGA_MPTxPinPullUpDown(true, false));
    assert(!PIOS_RE1FPGA_SetBuzzerType(PIOS_RE1FPGA_BUZZER_AC)); assert(!PIOS_RE1FPGA_Buzzer(true));
    assert(hw.regs[1] == 0x4f && hw.regs[2] == 1);
    hw.fail_transfer = true; unsigned calls = hw.transfers;
    assert(PIOS_RE1FPGA_SetSyncThreshold(45) != 0); released(); assert(hw.regs[5] == 30);
    hw.fail_transfer = false; assert(!PIOS_RE1FPGA_SetSyncThreshold(45));
    assert(hw.transfers == calls + 2 && hw.regs[5] == 45); /* Failed write was not cached. */
    uint8_t data[16] = {1, 2, 3}; assert(!PIOS_RE1FPGA_SetIRData(data, sizeof(data)));
    assert(hw.bulk_size == sizeof(data) && !memcmp(hw.bulk, data, sizeof(data))); released();
    hw.fail_claim = true; assert(PIOS_RE1FPGA_Buzzer(false) != 0); released();
    hw.fail_claim = false; hw.fail_clock = true; assert(PIOS_RE1FPGA_Buzzer(false) != 0); released();
    reset(); hw.cdone = false; assert(PIOS_RE1FPGA_Init(bus(), 2, &config, true) == -2);
    assert(hw.waited == 101 && hw.transfers == 0); released();
    reset(); hw.fail_transfer = true; assert(PIOS_RE1FPGA_Init(bus(), 2, &config, true) == -3); released();
    reset(); hw.fail_at = 9; assert(PIOS_RE1FPGA_Init(bus(), 2, &config, true) == -4); released();
    reset(); hw.drop_writes = true; assert(PIOS_RE1FPGA_Init(bus(), 2, &config, true) == -5); released();
    reset(); hw.regs[0] = 255; assert(PIOS_RE1FPGA_Init(bus(), 2, &config, true) == -4); released();
    for (unsigned i = 0; i < allocated; ++i) free(allocations[i]);
    puts("PASS: actual native FPGA driver, clock/CS ordering, revision/readback, OSD/serial/buzzer, error cleanup and failed-write retry");
}
