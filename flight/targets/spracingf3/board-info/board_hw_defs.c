/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original SPRACINGF3 tables translated from pinned Betaflight wiring.
 * Peripheral structures derived from dRonin (C) 2015-2016 and Tau Labs (C) 2012-2015.
 * Free software under GPL version 3 or later, without warranty.
 */
#include <pios.h>
#include "imported_profile.h"
#include <pios_annunc_priv.h>
#include <pios_flash_internal_priv.h>
#include <pios_flash_priv.h>
#include <pios_flashfs_logfs_priv.h>
#include <pios_i2c_priv.h>
#include <pios_servo_priv.h>
#include <pios_ppm_priv.h>
#include <pios_mpu.h>
#include <pios_usart_priv.h>
#include <pios_dsm_priv.h>
#include <pios_rtc_priv.h>
#include <pios_internal_adc_priv.h>
#include <pios_bmp280_priv.h>
#include <pios_ms5611_priv.h>
#include <pios_hmc5883_priv.h>

#define PIN(port, number, mode, pull) { .gpio = GPIO##port, .init = { \
    .GPIO_Pin = GPIO_Pin_##number, .GPIO_Mode = GPIO_Mode_##mode, .GPIO_Speed = GPIO_Speed_50MHz, \
    .GPIO_OType = GPIO_OType_PP, .GPIO_PuPd = GPIO_PuPd_##pull }, .pin_source = GPIO_PinSource##number }
#define IRQ(number) { .init = { .NVIC_IRQChannel = number, \
    .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_MID, .NVIC_IRQChannelCmd = ENABLE } }
static const struct pios_annunc annunciators[] = {
    [PIOS_LED_HEARTBEAT] = { .pin = PIN(B, 3, OUT, NOPULL), .active_high = false },
    [PIOS_LED_ALARM] = { .pin = PIN(B, 3, OUT, NOPULL), .active_high = false },
    [PIOS_ANNUNCIATOR_BUZZER] = { .pin = PIN(C, 15, OUT, NOPULL), .active_high = false },
};
static const struct pios_annunc_cfg pios_annunc_cfg = { annunciators, NELEMENTS(annunciators) };
const struct pios_annunc_cfg *PIOS_BOARD_HW_DEFS_GetLedCfg(uint32_t rev) { return &pios_annunc_cfg; }
static const struct pios_flash_internal_cfg flash_internal_cfg = {};
uintptr_t pios_internal_flash_id;
static const struct pios_flash_sector_range sectors[] = {
    { .base_sector = 0, .last_sector = 127, .sector_size = FLASH_SECTOR_2KB },
};
static const struct pios_flash_chip flash_chip = {
    .driver = &pios_internal_flash_driver, .chip_id = &pios_internal_flash_id, .page_size = 16,
    .sector_blocks = sectors, .num_blocks = NELEMENTS(sectors),
};
static const struct pios_flash_partition partitions[] = {
    { .label = FLASH_PARTITION_LABEL_SETTINGS, .chip_desc = &flash_chip, .first_sector = 2,
      .last_sector = 9, .chip_offset = 4096, .size = 16384 },
    { .label = FLASH_PARTITION_LABEL_AUTOTUNE, .chip_desc = &flash_chip, .first_sector = 10,
      .last_sector = 17, .chip_offset = 20480, .size = 16384 },
    { .label = FLASH_PARTITION_LABEL_FW, .chip_desc = &flash_chip, .first_sector = 18,
      .last_sector = 127, .chip_offset = 36864, .size = 225280 },
};
const struct pios_flash_partition *PIOS_BOARD_HW_DEFS_GetPartitionTable(uint32_t rev, uint32_t *count)
{ *count = NELEMENTS(partitions); return partitions; }
static const struct flashfs_logfs_cfg settings_fs = { .fs_magic = 0x53524633, .arena_size = 8192, .slot_size = 256 };

static pios_i2c_t sensor_bus;
static const struct pios_i2c_adapter_cfg sensor_bus_cfg = {
    .regs = I2C1, .remap = GPIO_AF_4,
    .init = { .I2C_Mode = I2C_Mode_I2C, .I2C_Ack = I2C_Ack_Enable,
        .I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit,
        .I2C_AnalogFilter = I2C_AnalogFilter_Enable, .I2C_Timing = 0x00310309 },
    .transfer_timeout_ms = 50, .scl = IMPORTED_I2C_SCL, .sda = IMPORTED_I2C_SDA,
    .event = IRQ(I2C1_EV_IRQn), .error = IRQ(I2C1_ER_IRQn),
};
void I2C1_EV_EXTI23_IRQHandler(void) { PIOS_I2C_EV_IRQ_Handler(sensor_bus); }
void I2C1_ER_IRQHandler(void) { PIOS_I2C_ER_IRQ_Handler(sensor_bus); }
static const struct pios_exti_cfg imu_exti __exti_config = {
    .vector = PIOS_MPU_IRQHandler, .line = EXTI_Line13, .pin = PIN(C, 13, IN, NOPULL),
    .irq = IRQ(EXTI15_10_IRQn),
    .exti = { .init = { .EXTI_Line = EXTI_Line13, .EXTI_Mode = EXTI_Mode_Interrupt,
        .EXTI_Trigger = EXTI_Trigger_Rising, .EXTI_LineCmd = ENABLE } },
};
static const struct pios_mpu_cfg imu_cfg = {
    .exti_cfg = &imu_exti, .default_samplerate = 1000, .orientation = IMPORTED_IMU_ORIENTATION,
};
static const TIM_TimeBaseInitTypeDef motor_base = { .TIM_Prescaler = 71, .TIM_ClockDivision = TIM_CKD_DIV1,
    .TIM_CounterMode = TIM_CounterMode_Up, .TIM_Period = 1000000 / PIOS_SERVO_UPDATE_HZ - 1 };
static const struct pios_tim_clock_cfg motor_clocks[] = {
    { .timer = TIM16, .time_base_init = &motor_base, .irq = IRQ(TIM1_UP_TIM16_IRQn) },
    { .timer = TIM17, .time_base_init = &motor_base, .irq = IRQ(TIM1_TRG_COM_TIM17_IRQn) },
    { .timer = TIM4, .time_base_init = &motor_base, .irq = IRQ(TIM4_IRQn) },
    { .timer = TIM15, .time_base_init = &motor_base, .irq = IRQ(TIM1_BRK_TIM15_IRQn) },
    { .timer = TIM2, .time_base_init = &motor_base, .irq = IRQ(TIM2_IRQn) },
};
static const struct pios_tim_channel motor_channels[] = IMPORTED_MOTOR_CHANNELS;
static const struct pios_servo_cfg servo_cfg = {
    .tim_oc_init = { .TIM_OCMode = TIM_OCMode_PWM1, .TIM_OutputState = TIM_OutputState_Enable,
        .TIM_OutputNState = TIM_OutputNState_Disable, .TIM_OCPolarity = TIM_OCPolarity_High,
        .TIM_OCNPolarity = TIM_OCNPolarity_High },
    .channels = motor_channels, .num_channels = NELEMENTS(motor_channels),
};
static const struct pios_tim_channel ppm_channels[] = {
    { .timer = TIM2, .timer_chan = TIM_Channel_1, .remap = GPIO_AF_1, .pin = PIN(A, 0, AF, UP) },
};
static const struct pios_ppm_cfg ppm_cfg = {
    .tim_ic_init = { .TIM_ICPolarity = TIM_ICPolarity_Rising, .TIM_ICSelection = TIM_ICSelection_DirectTI,
        .TIM_ICPrescaler = TIM_ICPSC_DIV1 }, .channels = ppm_channels, .num_channels = NELEMENTS(ppm_channels),
};
static const struct pios_usart_cfg uart1_cfg = { .regs = USART1, .remap = GPIO_AF_7,
    .irq = IRQ(USART1_IRQn), .rx = IMPORTED_UART1_RX, .tx = IMPORTED_UART1_TX };
static const struct pios_usart_cfg uart2_cfg = { .regs = USART2, .remap = GPIO_AF_7,
    .irq = IRQ(USART2_IRQn), .rx = IMPORTED_UART2_RX, .tx = IMPORTED_UART2_TX };
static const struct pios_usart_cfg uart3_cfg = { .regs = USART3, .remap = GPIO_AF_7,
    .irq = IRQ(USART3_IRQn), .rx = IMPORTED_UART3_RX, .tx = IMPORTED_UART3_TX };
static const struct pios_dsm_cfg dsm_cfg = { .bind = PIN(B, 11, OUT, NOPULL) };
static const struct pios_rtc_cfg rtc_cfg = { .clksrc = RCC_RTCCLKSource_HSE_Div32,
    .prescaler = 24, .irq = IRQ(RTC_WKUP_IRQn) };
void RTC_WKUP_IRQHandler(void) { PIOS_RTC_irq_handler(); }
static const struct pios_bmp280_cfg bmp_cfg = { .oversampling = BMP280_HIGH_RESOLUTION };
static const struct pios_ms5611_cfg ms_cfg = { .oversampling = MS5611_OSR_4096, .temperature_interleaving = 1 };
static const struct pios_hmc5883_cfg mag_cfg = { .M_ODR = PIOS_HMC5883_ODR_75,
    .Meas_Conf = PIOS_HMC5883_MEASCONF_NORMAL, .Gain = PIOS_HMC5883_GAIN_1_9,
    .Mode = PIOS_HMC5883_MODE_CONTINUOUS, .Default_Orientation = PIOS_HMC5883_TOP_90DEG };
static const struct pios_internal_adc_cfg adc_cfg = {
    .dma = { .irq = { .flags = DMA2_FLAG_TC1 | DMA2_FLAG_TE1 | DMA2_FLAG_HT1 | DMA2_FLAG_GL1,
        .init = { .NVIC_IRQChannel = DMA2_Channel1_IRQn,
            .NVIC_IRQChannelPreemptionPriority = PIOS_IRQ_PRIO_HIGH, .NVIC_IRQChannelCmd = ENABLE } },
        .rx = { .channel = DMA2_Channel1, .init = { .DMA_Priority = DMA_Priority_High } } },
    .half_flag = DMA2_IT_HT1, .full_flag = DMA2_IT_TC1, .oversampling = 32, .adc_pin_count = 3,
    .adc_pins = { {GPIOA, GPIO_Pin_4, ADC_Channel_1, true}, {GPIOA, GPIO_Pin_5, ADC_Channel_2, true},
        {GPIOB, GPIO_Pin_2, ADC_Channel_12, true} }, .adc_dev_master = ADC2,
};
