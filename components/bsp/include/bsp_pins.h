// components/bsp/include/bsp_pins.h
// FoloToy-Card hardware pins and parameters: [single source of truth]. Changing boards/hardware only requires editing this one file.
// Every item notes "why this value", so a later developer can judge whether it can be changed.
#pragma once

#include "driver/spi_master.h"
#include "driver/i2c_types.h"
#include "hal/adc_types.h"

// ============================================================================
// Display: ST7789P3 240x320, 4-line SPI
// ============================================================================
#define BSP_LCD_W            240
#define BSP_LCD_H            320
#define BSP_LCD_SPI_HOST     SPI2_HOST
#define BSP_LCD_MOSI         9
#define BSP_LCD_SCLK         8
#define BSP_LCD_CS           1
#define BSP_LCD_DC           20
// -1 = reset pin not wired to the MCU (hard-tied to 3.3V), esp_lcd_panel_reset() does a SWRESET soft reset.
#define BSP_LCD_RST          (-1)
#define BSP_LCD_BL           21          // backlight, LEDC PWM dimming
#define BSP_LCD_PCLK_HZ      (40 * 1000 * 1000)
// ST7789 SCK idles low, samples on the rising edge -> SPI mode 0.
#define BSP_LCD_SPI_MODE     0
// This panel needs color inversion from the factory (the reference routine TFT_init() unconditionally sends 0x21 INVON at the end).
// If the image looks like a negative after swapping panels, change this to 0.
#define BSP_LCD_INVERT_COLOR 1

// Backlight LEDC parameters
#define BSP_BL_LEDC_TIMER    LEDC_TIMER_0
#define BSP_BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BSP_BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BSP_BL_LEDC_RES      LEDC_TIMER_10_BIT
/* 20kHz: moved out of the audible range. The original 5kHz made the backlight boost circuit
   emit an audible buzz when master brightness < 100% (at 100% duty it is DC and silent). 20kHz@10bit is valid at 80MHz APB. */
#define BSP_BL_LEDC_FREQ_HZ  20000

// ============================================================================
// Buttons: three keys share one ADC pin, told apart by divider resistors
//
// Circuit: 3.3V -- external pull-up 10k --+-- ADC node (GPIO0 / ADC1_CH0)
//                              +-- button -- divider resistor -- GND
//
//   Up   : 0 ohm  -> 3.3 x 0/10k       =   0 mV
//   Down : 1k    -> 3.3 x 1k/11k      ~ 300 mV
//   OK   : 2.2k  -> 3.3 x 2.2k/12.2k  ~ 595 mV
//   Released : no path -> pulled up to 3300 mV
//
// WARNING: do not switch to the [internal pull-up]: about 45k ohm and imprecise, it would squeeze all three levels into 0~154mV and overlap with temperature drift.
//
// * What to do after changing divider/pull-up values: go to the demo Button page, which shows the current ADC voltage live;
//   hold the three keys one by one and note the readings, take the midpoint of adjacent levels as window boundaries, then edit BSP_BTN_MV below.
// ============================================================================
#define BSP_BTN_ADC_UNIT     ADC_UNIT_1
#define BSP_BTN_ADC_CHANNEL  ADC_CHANNEL_0    // GPIO0
#define BSP_BTN_COUNT        3

// Voltage window per key {min_mV, max_mV}; boundaries are the midpoints of adjacent levels.
// Measured with the ADC at 12 dB attenuation: UP about 5mV, DOWN about 300mV, OK about 600mV.
// Released >= 2500mV; anything above 1000mV is not treated as a key.
#define BSP_BTN_MV_TABLE  { {0, 150}, {150, 450}, {450, 1000} }

// ============================================================================
// I2C: ES8311 (audio codec) and CW2017 (fuel gauge) share one bus
// ============================================================================
#define BSP_I2C_PORT         I2C_NUM_0
#define BSP_I2C_SDA          10
#define BSP_I2C_SCL          7
#define BSP_I2C_ES8311_ADDR  0x18    // 7-bit address (8-bit form is 0x30)
#define BSP_I2C_CW2017_ADDR  0x63    // 7-bit address

// ============================================================================
// Audio: ES8311, I2S full duplex (same port, one tx and one rx, sharing MCLK/BCLK/WS)
// ============================================================================
#define BSP_I2S_PORT         I2S_NUM_0
#define BSP_I2S_MCLK         6
#define BSP_I2S_BCLK         5
#define BSP_I2S_WS           3
#define BSP_I2S_DOUT         2       // playback: MCU -> codec
#define BSP_I2S_DIN          4       // recording: codec -> MCU
// -1 = amplifier enable pin not wired to the MCU (always on). If your board wires it, fill in the corresponding GPIO.
#define BSP_I2S_PA_CTRL      (-1)
