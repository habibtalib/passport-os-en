// components/bsp/include/bsp_battery.h
// CellWise CW2017 fuel gauge: I2C 0x63, shares the bus with the ES8311.
// The BSP writes the vendor profile for this unit Yoteli 520mAh cell; the chip reports SOC% directly.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize. Calls bsp_i2c_init() internally (idempotent).
// Returns ESP_ERR_NOT_FOUND if the chip does not ACK - upper layers can use this to mark the item unavailable in the UI.
esp_err_t bsp_battery_init(void);

// Cached gauge readings, refreshed every 10s by the system monitor. Invalid or
// contradictory readings return -1; voltage is never converted into a fake SOC.
int bsp_battery_soc(void);
void bsp_battery_poll(void);
void bsp_battery_log_status(void);

// Battery voltage in mV; returns -1 on read failure.
int bsp_battery_mv(void);

#ifdef __cplusplus
}
#endif
