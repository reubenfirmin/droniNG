/* SPDX-License-Identifier: GPL-3.0-or-later; tests the complete production OSD driver. */
#include <pios.h>
#include <pios_max7456.h>
#include <pios_max7456_priv.h>
#include <pios_delay.h>
#include <stdio.h>

static uint8_t registers[128];
static bool claimed, selected;
static int absent, claim_failure, clock_failure, transfer_failure, allocation_failure;
static bool stuck_reset, stuck_clear, stuck_font;
static uint32_t now, speed;
static int allocations, transfers;
void *PIOS_malloc(size_t size) { if (allocation_failure) return NULL; ++allocations; return malloc(size); }
void PIOS_free(void *memory) { assert(memory); --allocations; free(memory); }
uint32_t PIOS_DELAY_GetuS(void) { return now; }
int32_t PIOS_DELAY_WaituS(uint32_t us) { now += us; return 0; }
void PIOS_Thread_Sleep(uint32_t ms) { now += ms * 1000; }
int32_t PIOS_SPI_ClaimBus(pios_spi_t id) {
    assert((uintptr_t)id == 1 && !claimed && !selected);
    if (claim_failure) return -1;
    claimed = true; return 0;
}
int32_t PIOS_SPI_ReleaseBus(pios_spi_t id) {
    assert((uintptr_t)id == 1 && claimed && !selected); claimed = false; return 0;
}
int32_t PIOS_SPI_SetClockSpeed(pios_spi_t id, uint32_t requested) {
    assert((uintptr_t)id == 1 && claimed && !selected && requested == 9000000);
    if (clock_failure) return 0;
    speed = requested; return requested;
}
int32_t PIOS_SPI_RC_PinSet(pios_spi_t id, uint32_t slave, bool high) {
    assert((uintptr_t)id == 1 && slave == 0 && claimed && speed == 9000000);
    assert(selected == !!high); selected = !high; return 0;
}
int32_t PIOS_SPI_TransferBlock(pios_spi_t id, const uint8_t *tx, uint8_t *rx, uint16_t size) {
    assert((uintptr_t)id == 1 && claimed && selected && speed == 9000000 && size == 2);
    ++transfers;
    if (transfer_failure) return -1;
    uint8_t reg = tx[0] & 0x7f;
    if (rx) {
        rx[0] = 0;
        rx[1] = absent ? (absent == 1 ? 0 : 0xff) : registers[reg];
    } else if (!absent) {
        registers[reg] = tx[1];
        if (reg == MAX7456_REG_VM0 && !stuck_reset) registers[reg] &= ~MAX7456_VM0_SRB_MASK;
        if (reg == MAX7456_REG_DMM && !stuck_clear) registers[reg] &= ~MAX7456_DMM_CLR_MASK;
        if (reg == MAX7456_REG_CMM) {
            if (stuck_font) registers[MAX7456_REG_STAT] |= 1u << 5;
            else registers[MAX7456_REG_STAT] &= ~(1u << 5);
        }
    }
    return 0;
}

static void reset_mock(void) {
    assert(!claimed && !selected && allocations == 0);
    memset(registers, 0, sizeof(registers));
    registers[MAX7456_REG_STAT] = 1; // PAL input.
    absent = claim_failure = clock_failure = transfer_failure = allocation_failure = 0;
    stuck_reset = stuck_clear = stuck_font = false;
    now = speed = transfers = 0;
}
static max7456_dev_t fitted(void) {
    max7456_dev_t dev = NULL;
    assert(PIOS_MAX7456_init(&dev, (pios_spi_t)1, 0) == 0 && dev);
    assert(!claimed && !selected && allocations == 1);
    uint8_t mode, right, bottom;
    PIOS_MAX7456_get_extents(dev, &mode, &right, &bottom, NULL, NULL);
    assert(mode == MAX7456_MODE_PAL && right == 29 && bottom == 15);
    return dev;
}
int main(void) {
    reset_mock(); max7456_dev_t dev = fitted();
    PIOS_MAX7456_puts(dev, 0, 0, "DroniNG", 0);
    assert(!claimed && !selected && !PIOS_MAX7456_stall_detect(dev));
    speed = 300000; // Another SPI3 device used the shared bus.
    PIOS_MAX7456_put(dev, 2, 3, 'X', 0);
    assert(speed == 9000000 && !claimed && !selected);
    PIOS_free(dev);
    for (int failure = 0; failure < 7; ++failure) {
        reset_mock(); dev = NULL;
        if (failure < 2) absent = failure + 1;
        if (failure == 2) claim_failure = 1;
        if (failure == 3) clock_failure = 1;
        if (failure == 4) transfer_failure = 1;
        if (failure == 5) allocation_failure = 1;
        if (failure == 6) stuck_reset = true;
        assert(PIOS_MAX7456_init(&dev, (pios_spi_t)1, 0) < 0 && dev == NULL);
        assert(!claimed && !selected && allocations == 0 && now <= 201000);
        if (failure == 2 || failure == 3 || failure == 5) assert(transfers == 0);
    }
    reset_mock(); stuck_clear = true; dev = NULL;
    assert(PIOS_MAX7456_init(&dev, (pios_spi_t)1, 0) < 0 && !dev && allocations == 0 && now <= 201000);
    reset_mock(); dev = fitted();
    uint8_t character[54] = {0};
    stuck_font = true; now = UINT32_MAX - 500;
    uint32_t start = now;
    PIOS_MAX7456_upload_char(dev, 1, character);
    assert((uint32_t)(now - start) >= 200000 && (uint32_t)(now - start) < 201000);
    stuck_font = false;
    assert(PIOS_MAX7456_stall_detect(dev));
    assert(!PIOS_MAX7456_stall_detect(dev));
    stuck_font = true; start = now;
    PIOS_MAX7456_download_char(dev, 1, character);
    assert((uint32_t)(now - start) >= 200000 && (uint32_t)(now - start) < 201000);
    stuck_font = false; assert(PIOS_MAX7456_stall_detect(dev));
    transfer_failure = 1;
    PIOS_MAX7456_puts(dev, 0, 0, "Interrupted", 0);
    assert(!claimed && !selected);
    transfer_failure = 0;
    assert(PIOS_MAX7456_stall_detect(dev));
    assert(!PIOS_MAX7456_stall_detect(dev));
    registers[MAX7456_REG_STAT] = 0; now = UINT32_MAX - 10000; start = now;
    PIOS_MAX7456_wait_vsync(dev);
    assert((uint32_t)(now - start) <= 81000);
    PIOS_free(dev);
    puts("PASS: actual MAX7456 driver; shared-bus clock ordering, missing chip, allocation/bus/transfer errors, bounded reset/clear/font/vsync and recovery");
    return 0;
}
