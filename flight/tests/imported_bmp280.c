/* SPDX-License-Identifier: GPL-3.0-or-later; complete production BMP280 driver. */
#include <pios.h>
#include <pios_bmp280_priv.h>
#include <pios_semaphore.h>
#include <stdio.h>

#include "imported_baro_configs.inc"

static uint8_t registers[256], address;
static bool claimed, selected, semaphore_taken;
static bool bus_failure, write_failure, read_failure, semaphore_failure;
static int control_writes;
static PIOS_SENSOR_Callback_t callback;
static void *callback_context;
struct pios_semaphore { int unused; };
static struct pios_semaphore semaphore;

void *PIOS_malloc(size_t size) { return malloc(size); }
void PIOS_free(void *memory) { free(memory); }
struct pios_semaphore *PIOS_Semaphore_Create(void) { return &semaphore; }
bool PIOS_Semaphore_Take(struct pios_semaphore *value, uint32_t timeout)
{
    (void)timeout;
    assert(value == &semaphore && !semaphore_taken);
    if (semaphore_failure) return false;
    semaphore_taken = true; return true;
}
bool PIOS_Semaphore_Give(struct pios_semaphore *value)
{
    assert(value == &semaphore && semaphore_taken);
    semaphore_taken = false; return true;
}
int32_t PIOS_DELAY_WaituS(uint32_t us) { (void)us; return 0; }
int32_t PIOS_DELAY_WaitmS(uint32_t ms) { (void)ms; return 0; }
int32_t PIOS_SENSORS_RegisterCallback(enum pios_sensor_type type,
    PIOS_SENSOR_Callback_t value, void *context)
{
    assert(type == PIOS_SENSOR_BARO);
    callback = value; callback_context = context; return 0;
}
int32_t PIOS_SPI_ClaimBus(pios_spi_t id)
{
    assert((uintptr_t)id == 1 && !claimed && !selected);
    if (bus_failure) return -1;
    claimed = true; return 0;
}
int32_t PIOS_SPI_ReleaseBus(pios_spi_t id)
{
    assert((uintptr_t)id == 1 && claimed && !selected);
    claimed = false; return 0;
}
int32_t PIOS_SPI_SetClockSpeed(pios_spi_t id, uint32_t speed)
{
    assert((uintptr_t)id == 1 && claimed && !selected && speed <= 10000000);
    return speed;
}
int32_t PIOS_SPI_RC_PinSet(pios_spi_t id, uint32_t slave, bool high)
{
    assert((uintptr_t)id == 1 && slave == 1 && claimed && selected == high);
    selected = !high; return 0;
}
static void raw_sample(uint8_t reg, uint32_t value)
{
    registers[reg] = value >> 12;
    registers[reg + 1] = value >> 4;
    registers[reg + 2] = value << 4;
}
int32_t PIOS_SPI_TransferBlock(pios_spi_t id, const uint8_t *tx, uint8_t *rx, uint16_t size)
{
    assert((uintptr_t)id == 1 && claimed && selected);
    if (tx && size == 1) { address = tx[0]; return 0; }
    if (rx) {
        if (read_failure) return -1;
        memcpy(rx, registers + address, size); return 0;
    }
    assert(tx && size == 2 && !(tx[0] & 0x80));
    if (write_failure) return -1;
    uint8_t reg = tx[0] | 0x80;
    registers[reg] = tx[1];
    if (reg == 0xf4) {
        ++control_writes;
        // BMP280 skips conversions when either oversampling field is zero.
        raw_sample(0xf7, (tx[1] & 0x1c) ? 415148 : 0x80000);
        raw_sample(0xfa, (tx[1] & 0xe0) ? 519888 : 0x80000);
    }
    return 0;
}
static bool sample(struct pios_sensor_baro_data *data)
{
    int next;
    bool ok = callback(callback_context, data, 0, &next);
    assert(!claimed && !selected && !semaphore_taken);
    assert(next > 0 && next < 100);
    return ok;
}
int main(void)
{
    // Bosch's reference compensation vector, with pressure in kPa.
    const int calibration[] = {27504, 26435, -1000, 36477, -10685, 3024,
                              2855, 140, -7, 15500, -14600, 6000};
    for (unsigned i = 0; i < sizeof(calibration) / sizeof(calibration[0]); ++i) {
        registers[0x88 + i * 2] = calibration[i] & 0xff;
        registers[0x89 + i * 2] = (uint16_t)calibration[i] >> 8;
    }
    const struct pios_bmp280_cfg invalid[] = {{3}, {0x20}, {0x10}};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        assert(PIOS_BMP280_SPI_Init(&invalid[i], (pios_spi_t)1, 1) == -12);
    const struct pios_bmp280_cfg *boards[] = {&omnibus_baro_cfg, &radix_baro_cfg};
    for (unsigned i = 0; i < sizeof(boards) / sizeof(boards[0]); ++i) {
        assert(PIOS_BMP280_SPI_Init(boards[i], (pios_spi_t)1, 1) == 0 && callback);
        struct pios_sensor_baro_data data;
        for (int n = 0; n < 100; ++n) {
            assert(sample(&data));
            assert(fabsf(data.temperature - 25.08f) < 0.02f);
            assert(fabsf(data.pressure - 100.653f) < 0.02f);
            assert(isfinite(data.altitude));
        }
        assert((registers[0xf4] & 0xe0) && (registers[0xf4] & 0x1c));
        assert((registers[0xf4] & 3) == 3 && control_writes >= 100);
        bus_failure = true; assert(!sample(&data)); bus_failure = false;
        write_failure = true; assert(!sample(&data)); write_failure = false;
        read_failure = true; assert(!sample(&data)); read_failure = false;
        semaphore_failure = true; assert(!sample(&data)); semaphore_failure = false;
        assert(sample(&data));
    }
    puts("PASS: actual Omnibus/RADIX BMP280 configuration and SPI driver; pressure/temperature conversion, reference compensation, repeated samples, skipped ADCs and bus/write/read/semaphore failures");
    return 0;
}
