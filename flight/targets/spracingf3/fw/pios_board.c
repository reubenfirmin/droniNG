/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "board_hw_defs.c"
#include <openpilot.h>
#include <pios_hal.h>
#include <pios_adc_priv.h>
#include <uavobjectsinit.h>
#include "hwspracingf3.h"
#include "boardprofile.h"
#include "modulesettings.h"

uintptr_t pios_uavo_settings_fs_id, pios_internal_adc_id, pios_com_openlog_logging_id;
const struct {
    char magic[4]; uint32_t schema; char profile[32], layout[32]; uint8_t hash[32];
} imported_identity __attribute__((used, section(".boardprofile"))) = {
    "BPrf", 1, IMPORTED_PROFILE_ID, IMPORTED_LAYOUT_ID, IMPORTED_PROFILE_HASH
};

static int sensor_read(uint8_t address, uint8_t reg, uint8_t *data, uint8_t count)
{
    const struct pios_i2c_txn transactions[] = {
        { .addr = address, .rw = PIOS_I2C_TXN_WRITE, .len = 1, .buf = &reg },
        { .addr = address, .rw = PIOS_I2C_TXN_READ, .len = count, .buf = data },
    };
    return PIOS_I2C_Transfer(sensor_bus, transactions, NELEMENTS(transactions));
}

static bool ms5611_present(void)
{
    uint16_t prom[8];
    uint8_t bytes[2];
    for (unsigned i = 0; i < NELEMENTS(prom); ++i) {
        if (sensor_read(0x77, 0xa0 + i * 2, bytes, 2)) return false;
        prom[i] = (bytes[0] << 8) | bytes[1];
    }
    return PIOS_MS5611_ValidatePROM(prom);
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
    HwSpracingF3Initialize();
    ModuleSettingsInitialize();
    uint16_t attempts = PIOS_IAP_ReadBootCount();
    if (attempts >= 3) {
        HwSpracingF3SetDefaults(HwSpracingF3Handle(), 0);
        ModuleSettingsSetDefaults(ModuleSettingsHandle(), 0);
        AlarmsSet(SYSTEMALARMS_ALARM_BOOTFAULT, SYSTEMALARMS_ALARM_CRITICAL);
    } else PIOS_IAP_WriteBootCount(attempts + 1);
    PIOS_RTC_Init(&rtc_cfg);
    PIOS_RESET_Clear();

    /* This port cannot be reassigned: it is also the built-in USB bridge. */
    PIOS_HAL_ConfigurePort(HWSHARED_PORTTYPES_TELEMETRY, &uart1_cfg, &pios_usart_com_driver,
        NULL, NULL, NULL, NULL, PIOS_LED_ALARM, NULL, 0, NULL);
    HwSpracingF3Data settings;
    HwSpracingF3Get(&settings);
    for (unsigned i = 0; i < NELEMENTS(motor_clocks); ++i) PIOS_TIM_InitClock(&motor_clocks[i]);
    PIOS_Servo_Init(&servo_cfg);
    const bool serial_receiver = settings.Uart3 != HWSPRACINGF3_UART3_DISABLED
        && settings.Uart3 != HWSPRACINGF3_UART3_GPS && settings.Uart3 != HWSPRACINGF3_UART3_MSP;
    const bool conflict = (settings.PPM == HWSPRACINGF3_PPM_ENABLED && serial_receiver)
        || (settings.Uart2 == HWSPRACINGF3_UART2_GPS && settings.Uart3 == HWSPRACINGF3_UART3_GPS)
        || (settings.Uart2 == HWSPRACINGF3_UART2_MSP && settings.Uart3 == HWSPRACINGF3_UART3_MSP);
    if (conflict) {
        /* Retain UART1 and live sensors so the user can correct a saved conflict. */
        AlarmsSet(SYSTEMALARMS_ALARM_SYSTEMCONFIGURATION, SYSTEMALARMS_ALARM_CRITICAL);
    } else {
        PIOS_HAL_ConfigurePort(settings.Uart2, &uart2_cfg, &pios_usart_com_driver,
            NULL, NULL, NULL, NULL, PIOS_LED_ALARM, NULL, 0, NULL);
        PIOS_HAL_ConfigurePort(settings.Uart3, &uart3_cfg, &pios_usart_com_driver,
            NULL, NULL, NULL, NULL, PIOS_LED_ALARM, &dsm_cfg, settings.DSMxMode, NULL);
        if (settings.PPM == HWSPRACINGF3_PPM_ENABLED)
            PIOS_HAL_ConfigurePort(HWSHARED_PORTTYPES_PPM, NULL, NULL,
                NULL, NULL, &ppm_cfg, NULL, PIOS_LED_ALARM, NULL, 0, NULL);
    }
    BoardProfileInitialize();
    BoardProfileData identity = { .SchemaVersion = 1, .SupportLevel = BOARDPROFILE_SUPPORTLEVEL_EXPERIMENTAL,
        .FPGARevision = 0xff };
    memcpy(identity.ProfileId, imported_identity.profile, sizeof(identity.ProfileId));
    memcpy(identity.LayoutId, imported_identity.layout, sizeof(identity.LayoutId));
    memcpy(identity.ProfileHash, imported_identity.hash, sizeof(identity.ProfileHash));
    BoardProfileSet(&identity);
    PIOS_SENSORS_Init();
    if (PIOS_I2C_Init(&sensor_bus, &sensor_bus_cfg) || PIOS_I2C_CheckClear(sensor_bus))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_I2C_INT);
    pios_mpu_dev_t imu = NULL;
    if (PIOS_MPU_I2C_Init(&imu, sensor_bus, &imu_cfg) || PIOS_MPU_GetType() != PIOS_MPU60X0
        || PIOS_MPU_SetGyroRange(PIOS_MPU_SCALE_2000_DEG) || PIOS_MPU_SetAccelRange(PIOS_MPU_SCALE_8G))
        PIOS_HAL_CriticalError(PIOS_LED_ALARM, PIOS_HAL_PANIC_IMU);
    uint8_t id[3];
    if (!sensor_read(0x76, 0xd0, id, 1) && id[0] == 0x58) {
        if (PIOS_BMP280_Init(&bmp_cfg, sensor_bus))
            AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_WARNING);
    } else if (ms5611_present()) {
        if (PIOS_MS5611_Init(&ms_cfg, sensor_bus))
            AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_WARNING);
    }
    if (!sensor_read(0x1e, 0x0a, id, 3) && !memcmp(id, "H43", 3))
        if (PIOS_HMC5883_Init(sensor_bus, &mag_cfg))
            AlarmsSet(SYSTEMALARMS_ALARM_SENSORS, SYSTEMALARMS_ALARM_WARNING);
    uintptr_t adc;
    PIOS_INTERNAL_ADC_Init(&adc, &adc_cfg);
    PIOS_ADC_Init(&pios_internal_adc_id, &pios_internal_adc_driver, adc);
    if (!(CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk)) PIOS_WDG_Init();
}
