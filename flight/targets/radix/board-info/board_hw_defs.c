/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native tables derived from dRonin/Tau Labs/BrainFPV RE1 definitions,
 * @author dRonin, http://dRonin.org/, Copyright (C) 2016
 * @author Tau Labs, http://taulabs.org, Copyright (C) 2012-2015
 * @author BrainFPV, Copyright (C) 2015
 */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Additional note on redistribution: The copyright and license notices above
 * must be maintained in each individual source file that is a derivative work
 * of this source file; otherwise redistribution is prohibited.
 */
#include <pios.h>
#include "imported_profile.h"
#include "../../common/brainfpv/board_hw_defs.c"
#include <pios_annunc_priv.h>
#include <pios_flash_internal_priv.h>
#include <pios_flash_priv.h>
#include <pios_flashfs_logfs_priv.h>
#include <pios_spi_priv.h>
#include <pios_servo_priv.h>
#include <pios_bmi160.h>
#include <pios_bmp280_priv.h>

static const struct pios_annunc pios_annuncs[] = {
    [PIOS_LED_HEARTBEAT] = { .pin = { .gpio = GPIOA, .init = {
        .GPIO_Pin = GPIO_Pin_4, .GPIO_Mode = GPIO_Mode_OUT, .GPIO_Speed = GPIO_Speed_2MHz,
        .GPIO_OType = GPIO_OType_PP, .GPIO_PuPd = GPIO_PuPd_UP } }, .active_high = false },
    [PIOS_LED_ALARM] = { .pin = { .gpio = GPIOA, .init = {
        .GPIO_Pin = GPIO_Pin_4, .GPIO_Mode = GPIO_Mode_OUT, .GPIO_Speed = GPIO_Speed_2MHz,
        .GPIO_OType = GPIO_OType_PP, .GPIO_PuPd = GPIO_PuPd_UP } }, .active_high = false },
    [PIOS_ANNUNCIATOR_BUZZER] = { .handler = PIOS_RE1FPGA_Buzzer },
};
static const struct pios_annunc_cfg pios_annunc_cfg = { pios_annuncs, NELEMENTS(pios_annuncs) };
const struct pios_annunc_cfg *PIOS_BOARD_HW_DEFS_GetLedCfg(uint32_t rev) { return &pios_annunc_cfg; }

static const struct pios_spi_cfg pios_spi_shared_cfg = {
    .regs = SPI3, .remap = GPIO_AF_SPI3,
    .init = { .SPI_Mode = SPI_Mode_Master, .SPI_Direction = SPI_Direction_2Lines_FullDuplex,
        .SPI_DataSize = SPI_DataSize_8b, .SPI_NSS = SPI_NSS_Soft, .SPI_FirstBit = SPI_FirstBit_MSB,
        .SPI_CRCPolynomial = 7, .SPI_CPOL = SPI_CPOL_High, .SPI_CPHA = SPI_CPHA_2Edge,
        .SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16 },
    .sclk = IMPORTED_SCK, .miso = IMPORTED_MISO, .mosi = IMPORTED_MOSI,
    .slave_count = 3, .ssel = { IMPORTED_IMU_CS, IMPORTED_BARO_CS, IMPORTED_FPGA_CS },
};
const struct pios_re1fpga_cfg pios_re1fpga_cfg = {
    .mco_pin = IMPORTED_FPGA_CLOCK, .rst_pin = IMPORTED_FPGA_RESET,
    .cdone_pin = IMPORTED_FPGA_CDONE, .cresetb_pin = IMPORTED_FPGA_CRESET,
    .spi_hz = 2812500, .sync_threshold = 30,
};
static const struct pios_flash_internal_cfg flash_internal_cfg = {};
uintptr_t pios_internal_flash_id;
static const struct pios_flash_sector_range internal_sectors[] = {
    { .base_sector = 0, .last_sector = 3, .sector_size = FLASH_SECTOR_16KB },
    { .base_sector = 4, .last_sector = 4, .sector_size = FLASH_SECTOR_64KB },
    { .base_sector = 5, .last_sector = 7, .sector_size = FLASH_SECTOR_128KB },
};
static const struct pios_flash_chip internal_chip = {
    .driver = &pios_internal_flash_driver, .chip_id = &pios_internal_flash_id, .page_size = 16,
    .sector_blocks = internal_sectors, .num_blocks = NELEMENTS(internal_sectors),
};
static const struct pios_flash_partition partitions[] = {
    { .label = FLASH_PARTITION_LABEL_SETTINGS, .chip_desc = &internal_chip,
      .first_sector = 1, .last_sector = 2, .chip_offset = 0x4000, .size = 0x8000 },
    { .label = FLASH_PARTITION_LABEL_AUTOTUNE, .chip_desc = &internal_chip,
      .first_sector = 4, .last_sector = 4, .chip_offset = 0x10000, .size = 0x10000 },
    { .label = FLASH_PARTITION_LABEL_FW, .chip_desc = &internal_chip,
      .first_sector = 5, .last_sector = 7, .chip_offset = 0x20000, .size = 0x60000 },
};
const struct pios_flash_partition *PIOS_BOARD_HW_DEFS_GetPartitionTable(uint32_t rev, uint32_t *count)
{ *count = NELEMENTS(partitions); return partitions; }
static const struct flashfs_logfs_cfg settings_fs = { .fs_magic = 0x52445831, .arena_size = 0x4000, .slot_size = 256 };

static const TIM_TimeBaseInitTypeDef apb1_base = {
    .TIM_Prescaler = PIOS_PERIPHERAL_APB1_COUNTER_CLOCK / 1000000 - 1,
    .TIM_ClockDivision = TIM_CKD_DIV1, .TIM_CounterMode = TIM_CounterMode_Up,
    .TIM_Period = 1000000 / PIOS_SERVO_UPDATE_HZ - 1,
};
static const TIM_TimeBaseInitTypeDef apb2_base = {
    .TIM_Prescaler = PIOS_PERIPHERAL_APB2_COUNTER_CLOCK / 1000000 - 1,
    .TIM_ClockDivision = TIM_CKD_DIV1, .TIM_CounterMode = TIM_CounterMode_Up,
    .TIM_Period = 1000000 / PIOS_SERVO_UPDATE_HZ - 1,
};
/* Explicit entries keep IRQ/clock domains visible in review. */
static const struct pios_tim_clock_cfg motor_clocks[] = {
    { .timer = TIM5, .time_base_init = &apb1_base, .irq = { .init = {
        .NVIC_IRQChannel = TIM5_IRQn, .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID, .NVIC_IRQChannelCmd = ENABLE } } },
    { .timer = TIM1, .time_base_init = &apb2_base, .irq = { .init = {
        .NVIC_IRQChannel = TIM1_UP_TIM10_IRQn, .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID, .NVIC_IRQChannelCmd = ENABLE } } },
    { .timer = TIM2, .time_base_init = &apb1_base, .irq = { .init = {
        .NVIC_IRQChannel = TIM2_IRQn, .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID, .NVIC_IRQChannelCmd = ENABLE } } },
};
static const struct pios_tim_channel motor_channels[] = IMPORTED_MOTOR_CHANNELS;
static const struct pios_servo_cfg servo_cfg = {
    .tim_oc_init = { .TIM_OCMode = TIM_OCMode_PWM1, .TIM_OutputState = TIM_OutputState_Enable,
        .TIM_OutputNState = TIM_OutputNState_Disable, .TIM_Pulse = 0,
        .TIM_OCPolarity = TIM_OCPolarity_High, .TIM_OCNPolarity = TIM_OCNPolarity_High,
        .TIM_OCIdleState = TIM_OCIdleState_Reset, .TIM_OCNIdleState = TIM_OCNIdleState_Reset },
    .channels = motor_channels, .num_channels = NELEMENTS(motor_channels),
};
static const struct pios_exti_cfg imu_exti __exti_config = {
    .vector = PIOS_BMI160_IRQHandler, .line = EXTI_Line13,
    .pin = { .gpio = GPIOC, .init = { .GPIO_Pin = GPIO_Pin_13, .GPIO_Mode = GPIO_Mode_IN,
        .GPIO_Speed = GPIO_Speed_2MHz, .GPIO_OType = GPIO_OType_OD, .GPIO_PuPd = GPIO_PuPd_NOPULL } },
    .irq = { .init = { .NVIC_IRQChannel = EXTI15_10_IRQn,
        .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID, .NVIC_IRQChannelCmd = ENABLE } },
    .exti = { .init = { .EXTI_Line = EXTI_Line13, .EXTI_Mode = EXTI_Mode_Interrupt,
        .EXTI_Trigger = EXTI_Trigger_Rising, .EXTI_LineCmd = ENABLE } },
};
static const struct pios_bmi160_cfg imu_cfg = {
    .exti_cfg = &imu_exti, .orientation = IMPORTED_IMU_ORIENTATION,
    .odr = PIOS_BMI160_ODR_1600_Hz, .acc_range = PIOS_BMI160_RANGE_8G,
    .gyro_range = PIOS_BMI160_RANGE_2000DPS, .temperature_interleaving = 50,
};
static const struct pios_bmp280_cfg baro_cfg = { .oversampling = BMP280_HIGH_RESOLUTION };
