/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native tables translated from pinned Betaflight OMNIBUSF4SD wiring.
 * Common peripheral structures follow dRonin/Tau Labs definitions.
 * Copyright (C) 2015-2016 dRonin, http://dronin.org/
 * Copyright (C) 2012-2015 Tau Labs, http://taulabs.org/
 * This program is free software under GPL version 3 or later, without warranty.
 */
#include <pios.h>
#include "imported_profile.h"
#include <pios_annunc_priv.h>
#include <pios_flash_internal_priv.h>
#include <pios_flash_priv.h>
#include <pios_flashfs_logfs_priv.h>
#include <pios_spi_priv.h>
#include <pios_servo_priv.h>
#include <pios_mpu.h>
#include <pios_bmp280_priv.h>
#include <pios_usart_priv.h>
#include <pios_dsm_priv.h>
#include <pios_sbus_priv.h>
#include <pios_usb_priv.h>
#include <pios_usb_board_data_priv.h>
#include <pios_usb_desc_hid_cdc_priv.h>
#include <pios_usbhook.h>
#include <pios_usb_cdc_priv.h>
#include <pios_usb_hid_priv.h>
#include <pios_rtc_priv.h>
#include <pios_adc_priv.h>
#include <pios_internal_adc_priv.h>

#define PIN(port, number, mode, pull) { .gpio = GPIO##port, .init = { \
    .GPIO_Pin = GPIO_Pin_##number, .GPIO_Mode = GPIO_Mode_##mode, .GPIO_Speed = GPIO_Speed_50MHz, \
    .GPIO_OType = GPIO_OType_PP, .GPIO_PuPd = GPIO_PuPd_##pull }, .pin_source = GPIO_PinSource##number }
#define IRQ(number) { .init = { .NVIC_IRQChannel = number, \
    .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID, .NVIC_IRQChannelCmd = ENABLE } }
static const struct pios_annunc pios_annuncs[] = {
    [PIOS_LED_HEARTBEAT] = { .pin = PIN(B, 5, OUT, NOPULL), .active_high = false },
    [PIOS_LED_ALARM] = { .pin = PIN(B, 5, OUT, NOPULL), .active_high = false },
    [PIOS_ANNUNCIATOR_BUZZER] = { .pin = PIN(B, 4, OUT, UP), .active_high = false },
};
static const struct pios_annunc_cfg pios_annunc_cfg = { pios_annuncs, NELEMENTS(pios_annuncs) };
const struct pios_annunc_cfg *PIOS_BOARD_HW_DEFS_GetLedCfg(uint32_t rev) { return &pios_annunc_cfg; }

static const struct pios_flash_internal_cfg flash_internal_cfg = {};
uintptr_t pios_internal_flash_id;
static const struct pios_flash_sector_range internal_sectors[] = {
    { .base_sector = 0, .last_sector = 3, .sector_size = FLASH_SECTOR_16KB },
    { .base_sector = 4, .last_sector = 4, .sector_size = FLASH_SECTOR_64KB },
    { .base_sector = 5, .last_sector = 11, .sector_size = FLASH_SECTOR_128KB },
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
      .first_sector = 5, .last_sector = 11, .chip_offset = 0x20000, .size = 0xe0000 },
};
const struct pios_flash_partition *PIOS_BOARD_HW_DEFS_GetPartitionTable(uint32_t rev, uint32_t *count)
{ *count = NELEMENTS(partitions); return partitions; }
static const struct flashfs_logfs_cfg settings_fs = { .fs_magic = 0x4f425331, .arena_size = 0x4000, .slot_size = 256 };

#define SPI_INIT(polarity, phase) { .SPI_Mode = SPI_Mode_Master, .SPI_Direction = SPI_Direction_2Lines_FullDuplex, \
    .SPI_DataSize = SPI_DataSize_8b, .SPI_NSS = SPI_NSS_Soft, .SPI_FirstBit = SPI_FirstBit_MSB, \
    .SPI_CRCPolynomial = 7, .SPI_CPOL = polarity, .SPI_CPHA = phase, \
    .SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16 }
static const struct pios_spi_cfg pios_spi_imu_cfg = {
    .regs = SPI1, .remap = GPIO_AF_SPI1, .init = SPI_INIT(SPI_CPOL_High, SPI_CPHA_2Edge),
    .sclk = IMPORTED_IMU_SCK, .miso = IMPORTED_IMU_MISO, .mosi = IMPORTED_IMU_MOSI,
    .slave_count = 1, .ssel = { IMPORTED_IMU_CS },
};
static const struct pios_spi_cfg pios_spi_osd_cfg = {
    .regs = SPI3, .remap = GPIO_AF_SPI3, .init = SPI_INIT(SPI_CPOL_Low, SPI_CPHA_1Edge),
    .sclk = IMPORTED_OSD_SCK, .miso = IMPORTED_OSD_MISO, .mosi = IMPORTED_OSD_MOSI,
    .slave_count = 2, .ssel = { IMPORTED_OSD_CS, IMPORTED_BARO_CS },
};
static const TIM_TimeBaseInitTypeDef apb1_base = {
    .TIM_Prescaler = PIOS_PERIPHERAL_APB1_COUNTER_CLOCK / 1000000 - 1,
    .TIM_ClockDivision = TIM_CKD_DIV1, .TIM_CounterMode = TIM_CounterMode_Up,
    .TIM_Period = 1000000 / PIOS_SERVO_UPDATE_HZ - 1,
};
static const struct pios_tim_clock_cfg motor_clocks[] = {
    { .timer = TIM3, .time_base_init = &apb1_base, .irq = IRQ(TIM3_IRQn) },
    { .timer = TIM2, .time_base_init = &apb1_base, .irq = IRQ(TIM2_IRQn) },
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
    .vector = PIOS_MPU_IRQHandler, .line = EXTI_Line4, .pin = PIN(C, 4, IN, NOPULL),
    .irq = IRQ(EXTI4_IRQn),
    .exti = { .init = { .EXTI_Line = EXTI_Line4, .EXTI_Mode = EXTI_Mode_Interrupt,
        .EXTI_Trigger = EXTI_Trigger_Rising, .EXTI_LineCmd = ENABLE } },
};
static const struct pios_mpu_cfg imu_cfg = {
    .exti_cfg = &imu_exti, .orientation = IMPORTED_IMU_ORIENTATION, .default_samplerate = 1000,
};
static const struct pios_bmp280_cfg baro_cfg = { .oversampling = BMP280_HIGH_RESOLUTION };
static const struct pios_usart_cfg pios_usart1_cfg = {
    .regs = USART1, .remap = GPIO_AF_USART1, .irq = IRQ(USART1_IRQn),
    .rx = PIN(A, 10, AF, UP), .tx = PIN(A, 9, AF, UP),
};
static const struct pios_usart_cfg pios_usart3_cfg = {
    .regs = USART3, .remap = GPIO_AF_USART3, .irq = IRQ(USART3_IRQn),
    .rx = PIN(B, 11, AF, UP), .tx = PIN(B, 10, AF, UP),
};
static const struct pios_usart_cfg pios_usart6_cfg = {
    .regs = USART6, .remap = GPIO_AF_USART6, .irq = IRQ(USART6_IRQn),
    .rx = PIN(C, 7, AF, UP), .tx = PIN(C, 6, AF, UP),
};
static const struct pios_dsm_cfg pios_rxport3_dsm_cfg = { .bind = PIN(B, 11, OUT, NOPULL) };
static const struct pios_dsm_cfg pios_rxport6_dsm_cfg = { .bind = PIN(C, 7, OUT, NOPULL) };
static const struct pios_sbus_cfg pios_sbus3_cfg = {
    .inv = PIN(C, 9, OUT, DOWN), .gpio_inv_enable = Bit_SET, .gpio_inv_disable = Bit_RESET,
};
static const struct pios_sbus_cfg pios_sbus6_cfg = {
    .inv = PIN(C, 8, OUT, DOWN), .gpio_inv_enable = Bit_SET, .gpio_inv_disable = Bit_RESET,
};
static const struct pios_usb_cfg pios_usb_main_cfg = {
    .irq = { .init = { .NVIC_IRQChannel = OTG_FS_IRQn,
        .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_HIGHEST, .NVIC_IRQChannelCmd = ENABLE } },
    .vsense = PIN(C, 5, IN, NOPULL),
};
const struct pios_usb_cdc_cfg pios_usb_cdc_cfg = { .ctrl_if = 0, .ctrl_tx_ep = 2,
    .data_if = 1, .data_rx_ep = 3, .data_tx_ep = 3 };
const struct pios_usb_hid_cfg pios_usb_hid_cfg = { .data_if = 2, .data_rx_ep = 1, .data_tx_ep = 1 };
static const struct pios_rtc_cfg pios_rtc_main_cfg = {
    .clksrc = RCC_RTCCLKSource_HSE_Div16, .prescaler = 50, .irq = IRQ(RTC_WKUP_IRQn),
};
void RTC_WKUP_IRQHandler(void) { PIOS_RTC_irq_handler(); }
/* The same analog pads/channels use ADC1's native DMA backend. Betaflight uses ADC2. */
static struct pios_internal_adc_cfg pios_adc_cfg = {
    .adc_dev_master = ADC1,
    .dma = { .irq = { .flags = DMA_FLAG_TCIF4 | DMA_FLAG_TEIF4 | DMA_FLAG_HTIF4,
        .init = { .NVIC_IRQChannel = DMA2_Stream4_IRQn,
            .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID, .NVIC_IRQChannelCmd = ENABLE } },
        .rx = { .channel = DMA2_Stream4, .init = { .DMA_Channel = DMA_Channel_0,
            .DMA_PeripheralBaseAddr = (uint32_t)&ADC1->DR } } },
    .half_flag = DMA_IT_HTIF4, .full_flag = DMA_IT_TCIF4,
    .adc_pins = { { GPIOC, GPIO_Pin_1, ADC_Channel_11 }, { GPIOC, GPIO_Pin_2, ADC_Channel_12 },
        { GPIOA, GPIO_Pin_0, ADC_Channel_0 } }, .adc_pin_count = 3,
};
void DMA2_Stream4_IRQHandler(void) { PIOS_INTERNAL_ADC_DMA_Handler(); }
