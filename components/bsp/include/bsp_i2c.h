// components/bsp/include/bsp_i2c.h
// ES8311 (0x18) and CW2017 (0x63) share one I2C bus. This module owns the bus handle on its own,
// and both drivers request it from here - avoiding the implicit ordering dependency of "whoever initializes first creates the bus".
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the shared bus. Idempotent: repeated calls just return ESP_OK, so each driver init can call it freely.
esp_err_t bsp_i2c_init(void);

// Get the shared bus handle. Returns NULL if not initialized.
i2c_master_bus_handle_t bsp_i2c_bus(void);

// Scan 0x08..0x77 and print every device that ACKs. Very useful for troubleshooting "is the chip not soldered / is the address right".
// Scans directly on the real bus; do not open a temporary bus - see the comment on bsp_i2c_scan() in bsp_i2c.c for why.
esp_err_t bsp_i2c_scan(void);

#ifdef __cplusplus
}
#endif
