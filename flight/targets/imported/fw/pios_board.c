/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native imported-board startup, derived from dRonin/Tau Labs/BrainFPV RE1
 * initialization.
 * @author dRonin, http://dRonin.org/, Copyright (C) 2016
 * @author Tau Labs, http://taulabs.org, Copyright (C) 2012-2013
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
#include "board_hw_defs.c"
#include <openpilot.h>
#include <pios_hal.h>
#include <uavobjectsinit.h>
#include "hwimported.h"
#include "boardprofile.h"
#include "modulesettings.h"
#include "flightbatterysettings.h"

uintptr_t pios_uavo_settings_fs_id, pios_internal_adc_id;
uintptr_t pios_com_logging_id, pios_com_openlog_logging_id;
#if defined(PIOS_INCLUDE_MAX7456)
max7456_dev_t pios_max7456_id;
#endif
/* Immutable full profile identity lives in sector 3 alongside board info. */
const struct {
    char magic[4]; uint32_t schema; char profile[32], layout[32]; uint8_t hash[32];
} imported_identity __attribute__((used, section(".boardprofile"))) = {
    "BPrf", 1, IMPORTED_PROFILE_ID, IMPORTED_LAYOUT_ID, IMPORTED_PROFILE_HASH
};

#if defined(IMPORTED_PROFILE_RADIX)
static void settings_changed(const UAVObjEvent *ev, void *ctx, void *obj, int len)
{
    HwImportedData *settings = obj;
    if (PIOS_RE1FPGA_SetSyncThreshold(settings->VideoSyncThreshold) ||
        PIOS_RE1FPGA_SetBuzzerType(settings->BuzzerType == HWIMPORTED_BUZZERTYPE_AC
            ? PIOS_RE1FPGA_BUZZER_AC : PIOS_RE1FPGA_BUZZER_DC))
        AlarmsSet(SYSTEMALARMS_ALARM_SYSTEMCONFIGURATION, SYSTEMALARMS_ALARM_CRITICAL);
}
#endif

static void battery_settings_init(void)
{
    FlightBatterySettingsInitialize();
    /* Supply manufacturer calibration only when no saved native settings exist.
     * Native factors are mV/V and mV/A, unlike Betaflight's integer scales. */
    if (UAVObjLoad(FlightBatterySettingsHandle(), 0) != 0) {
        FlightBatterySettingsData battery;
        FlightBatterySettingsGet(&battery);
        battery.SensorCalibrationFactor[FLIGHTBATTERYSETTINGS_SENSORCALIBRATIONFACTOR_VOLTAGE] = IMPORTED_ADC_VOLTAGE_FACTOR;
        battery.SensorCalibrationFactor[FLIGHTBATTERYSETTINGS_SENSORCALIBRATIONFACTOR_CURRENT] = IMPORTED_ADC_CURRENT_FACTOR;
        FlightBatterySettingsSet(&battery);
    }
}

void PIOS_Board_Init(void)
{
    PIOS_ANNUNC_Init(&pios_annunc_cfg);
    PIOS_IAP_Init();
    if (PIOS_Flash_Internal_Init(&pios_internal_flash_id, &flash_internal_cfg))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_FLASH);
    PIOS_FLASH_register_partition_table(partitions, NELEMENTS(partitions));
    if (PIOS_FLASHFS_Logfs_Init(&pios_uavo_settings_fs_id, &settings_fs, FLASH_PARTITION_LABEL_SETTINGS))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_FILESYS);
    HwImportedInitialize();
    ModuleSettingsInitialize();
    uint16_t attempts = PIOS_IAP_ReadBootCount();
    if (attempts >= 3) {
        HwImportedSetDefaults(HwImportedHandle(), 0);
        ModuleSettingsSetDefaults(ModuleSettingsHandle(), 0);
        AlarmsSet(SYSTEMALARMS_ALARM_BOOTFAULT, SYSTEMALARMS_ALARM_CRITICAL);
    } else {
        PIOS_IAP_WriteBootCount(attempts + 1);
        AlarmsClear(SYSTEMALARMS_ALARM_BOOTFAULT);
    }
    PIOS_RTC_Init(&pios_rtc_main_cfg);
    PIOS_RESET_Clear();

    HwImportedData settings;
    HwImportedGet(&settings);
    PIOS_USB_BOARD_DATA_Init();
    PIOS_USB_DESC_HID_CDC_Init();
    uintptr_t usb;
    PIOS_USB_Init(&usb, &pios_usb_main_cfg);
    PIOS_HAL_ConfigureCDC(settings.USB_VCPPort, usb, &pios_usb_cdc_cfg);
    /* Always retain a recoverable native telemetry interface. */
    PIOS_HAL_ConfigureHID(HWIMPORTED_USB_HIDPORT_USBTELEMETRY, usb, &pios_usb_hid_cfg);
    PIOS_USBHOOK_Activate();

    pios_spi_t spi;
#if defined(IMPORTED_PROFILE_RADIX)
    if (PIOS_SPI_Init(&spi, &pios_spi_shared_cfg))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_IMU);
    if (PIOS_RE1FPGA_Init(spi, 2, &pios_re1fpga_cfg, true))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_CAN);
    HwImportedConnectCallback(settings_changed);
    settings_changed(NULL, NULL, &settings, sizeof(settings));
#else
    pios_spi_t osd_spi;
    if (PIOS_SPI_Init(&spi, &pios_spi_imu_cfg) || PIOS_SPI_Init(&osd_spi, &pios_spi_osd_cfg))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_SPI);
    if (PIOS_MAX7456_init(&pios_max7456_id, osd_spi, 0))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_OSD);
#endif

    BoardProfileInitialize();
    BoardProfileData identity = { .SchemaVersion = 1, .SupportLevel = BOARDPROFILE_SUPPORTLEVEL_EXPERIMENTAL,
        .FPGARevision = 0xff };
#if defined(IMPORTED_PROFILE_RADIX)
    identity.FPGARevision = PIOS_RE1FPGA_GetHWRevision();
#endif
    memcpy(identity.ProfileId, imported_identity.profile, sizeof(identity.ProfileId));
    memcpy(identity.LayoutId, imported_identity.layout, sizeof(identity.LayoutId));
    memcpy(identity.ProfileHash, imported_identity.hash, sizeof(identity.ProfileHash));
    BoardProfileSet(&identity);

#if defined(IMPORTED_PROFILE_RADIX)
    if (PIOS_RE1FPGA_SerialRxInvert(settings.Receiver == HWIMPORTED_RECEIVER_SBUS))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_CAN);
    PIOS_HAL_ConfigurePort(settings.Receiver, &pios_usart3_cfg, &pios_usart_com_driver,
        NULL, NULL, NULL, NULL, PIOS_LED_ALARM, &pios_rxport_dsm_cfg, settings.DSMxMode, NULL);
    PIOS_HAL_ConfigurePort(settings.Telemetry, &pios_usart1_cfg, &pios_usart_com_driver,
        NULL, NULL, NULL, NULL, PIOS_LED_ALARM, NULL, 0, NULL);
    PIOS_HAL_ConfigurePort(settings.GPS, &pios_usart4_cfg, &pios_usart_com_driver,
        NULL, NULL, NULL, NULL, PIOS_LED_ALARM, NULL, 0, NULL);
    if (settings.ReceiverTelemetry != HWIMPORTED_RECEIVERTELEMETRY_DISABLED) {
        bool sport = settings.ReceiverTelemetry == HWIMPORTED_RECEIVERTELEMETRY_FRSKYSPORTTELEMETRY;
        if (PIOS_RE1FPGA_MPTxPinMode(sport, true) || PIOS_RE1FPGA_MPTxPinPullUpDown(sport, false))
            PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_CAN);
        PIOS_HAL_ConfigurePort(settings.ReceiverTelemetry, &pios_usart6_cfg, &pios_usart_com_driver,
            NULL, NULL, NULL, NULL, PIOS_LED_ALARM, NULL, 0, NULL);
    }
#else
    bool receiver3 = settings.ReceiverPort == HWIMPORTED_RECEIVERPORT_UART3;
    PIOS_HAL_ConfigurePort(settings.Receiver, receiver3 ? &pios_usart3_cfg : &pios_usart6_cfg,
        &pios_usart_com_driver, NULL, NULL, NULL, NULL, PIOS_LED_ALARM,
        receiver3 ? &pios_rxport3_dsm_cfg : &pios_rxport6_dsm_cfg, settings.DSMxMode,
        receiver3 ? &pios_sbus3_cfg : &pios_sbus6_cfg);
    PIOS_HAL_ConfigurePort(settings.GPS, receiver3 ? &pios_usart6_cfg : &pios_usart3_cfg,
        &pios_usart_com_driver, NULL, NULL, NULL, NULL, PIOS_LED_ALARM, NULL, 0,
        receiver3 ? &pios_sbus6_cfg : &pios_sbus3_cfg);
    PIOS_HAL_ConfigurePort(settings.Telemetry, &pios_usart1_cfg, &pios_usart_com_driver,
        NULL, NULL, NULL, NULL, PIOS_LED_ALARM, NULL, 0, NULL);
#endif
    for (unsigned i = 0; i < NELEMENTS(motor_clocks); ++i) PIOS_TIM_InitClock(&motor_clocks[i]);
    PIOS_Servo_Init(&servo_cfg);
    PIOS_SENSORS_Init();
#if defined(IMPORTED_PROFILE_RADIX)
    if (PIOS_BMI160_Init(spi, 0, &imu_cfg, false))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_IMU);
    if (PIOS_BMP280_SPI_Init(&baro_cfg, spi, 1))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_BARO);
#else
    /* The MPU initializer allocates only when the incoming handle is null. */
    pios_mpu_dev_t imu = NULL;
    if (PIOS_MPU_SPI_Init(&imu, spi, 0, &imu_cfg) ||
        (PIOS_MPU_GetType() != PIOS_MPU60X0 && PIOS_MPU_GetType() != PIOS_MPU6500 && PIOS_MPU_GetType() != PIOS_ICM20608G) ||
        PIOS_MPU_SetGyroRange(PIOS_MPU_SCALE_2000_DEG) || PIOS_MPU_SetAccelRange(PIOS_MPU_SCALE_8G))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_IMU);
    if (PIOS_MPU_SetGyroBandwidth(188))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_IMU);
    if (PIOS_BMP280_SPI_Init(&baro_cfg, osd_spi, 1))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_BARO);
#endif
    uintptr_t adc;
    PIOS_INTERNAL_ADC_Init(&adc, &pios_adc_cfg);
    PIOS_ADC_Init(&pios_internal_adc_id, &pios_internal_adc_driver, adc);
    battery_settings_init();
    if (!(CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk)) PIOS_WDG_Init();
}
