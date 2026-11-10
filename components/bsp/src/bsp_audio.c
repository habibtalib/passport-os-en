// components/bsp/src/bsp_audio.c
// Ported from trae_card/components/platform/platform_esp32/src/audio_es8311.c
#include "bsp_audio.h"
#include "bsp_audio_volume.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

static const char *TAG = "bsp_audio";

static esp_codec_dev_handle_t s_dev;
static i2s_chan_handle_t      s_tx, s_rx;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_data_if_t *s_data;
static const audio_codec_if_t *s_codec;
static const audio_codec_gpio_if_t *s_gpio;
static bool s_tx_primed, s_rx_primed;
// Records the currently opened format, to decide "whether to close and reopen" (see the pitfall note in the header).
static uint32_t s_hz;
static uint8_t  s_bits, s_ch;
static bool     s_opened;
typedef enum { AUDIO_DUPLEX, AUDIO_CAPTURE, AUDIO_PLAYBACK, AUDIO_FEEDBACK } audio_profile_t;
static audio_profile_t s_profile;
static uint8_t  s_requested_volume = 100;
static bool s_feedback_owner;
static uint8_t s_before_feedback_volume;
static atomic_uchar s_master_volume = 80;

// Settings can change volume while short feedback opens/closes the codec.
// Serialize control operations; capture and playback remain full duplex.
static StaticSemaphore_t s_control_storage;
static SemaphoreHandle_t s_control_mutex;
static portMUX_TYPE s_control_init_lock = portMUX_INITIALIZER_UNLOCKED;
static void control_lock(void) {
    taskENTER_CRITICAL(&s_control_init_lock);
    if (!s_control_mutex) s_control_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_control_storage);
    taskEXIT_CRITICAL(&s_control_init_lock);
    xSemaphoreTakeRecursive(s_control_mutex, portMAX_DELAY);
}
static void control_unlock(void) { xSemaphoreGiveRecursive(s_control_mutex); }

static esp_err_t i2s_profile_init(audio_profile_t profile) {
    const bool tx = profile != AUDIO_CAPTURE;
    const bool rx = profile == AUDIO_CAPTURE || profile == AUDIO_DUPLEX;
    const bool compact = profile == AUDIO_CAPTURE || profile == AUDIO_FEEDBACK;
    i2s_chan_config_t chan = {
        .id = BSP_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = compact ? 4 : 6,
        .dma_frame_num = compact ? 160 : 240,
        // Capture: 40 ms at 16 kHz. Playback retains the existing 6*240 ring.
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t e = i2s_new_channel(&chan, tx ? &s_tx : NULL, rx ? &s_rx : NULL);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(e)); return e; }

    // The sample rate here is only used to build the channel; the actual rate is reconfigured on demand by esp_codec_dev_open().
    i2s_std_config_t std = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_BCLK, .ws = BSP_I2S_WS,
            .dout = tx ? BSP_I2S_DOUT : I2S_GPIO_UNUSED,
            .din = rx ? BSP_I2S_DIN : I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (s_tx && (e = i2s_channel_init_std_mode(s_tx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s tx init failed: %s", esp_err_to_name(e)); return e;
    }
    if (s_rx && (e = i2s_channel_init_std_mode(s_rx, &std)) != ESP_OK) {
        ESP_LOGE(TAG, "i2s rx init failed: %s", esp_err_to_name(e)); return e;
    }
    // Before reconfiguring, esp_codec_dev_open internally calls i2s_channel_disable first, and disable requires the channel to be
    // RUNNING; a freshly init-ed channel is READY, which logs a "channel has not been enabled yet" error.
    // So enable once here to make that disable legal (the codec is not configured yet, so no sound).
    if (s_tx) {
        e = i2s_channel_enable(s_tx);
        if (e != ESP_OK) return e;
        s_tx_primed = true;
    }
    if (s_rx) {
        e = i2s_channel_enable(s_rx);
        if (e != ESP_OK) return e;
        s_rx_primed = true;
    }
    return ESP_OK;
}

static esp_err_t audio_init(audio_profile_t profile) {
    if (s_dev) return s_profile == profile ? ESP_OK : ESP_ERR_INVALID_STATE;
    s_profile = profile;

    esp_err_t e = bsp_i2c_init();
    if (e != ESP_OK) return e;

    s_ctrl = audio_codec_new_i2c_ctrl(&(audio_codec_i2c_cfg_t){
        .port = BSP_I2C_PORT,
        .addr = BSP_I2C_ES8311_ADDR << 1,   // this interface wants the 8-bit address form
        .bus_handle = bsp_i2c_bus(),
    });
    if (!s_ctrl) {
        ESP_LOGE(TAG, "ES8311 control port creation failed - use bsp_i2c_scan() to check whether 0x%02X ACKs; "
                      "check SDA=GPIO%d / SCL=GPIO%d wiring and codec power",
                 BSP_I2C_ES8311_ADDR, BSP_I2C_SDA, BSP_I2C_SCL);
        return ESP_FAIL;
    }

    // Aligned with the official folo-ai-passport-xiaozhi (es8311_audio_codec.cc:64-73):
    // on init, soft-reset the ES8311 (REG 0x00 = 0x1F) and wait 5ms,
    // resetting the digital block, CSM clock state machine and modulator, eliminating digital current noise caused by leftover clock state from power-up/warm reboot.
    uint8_t reset_val = 0x1F;
    s_ctrl->write_reg(s_ctrl, 0x00, 1, &reset_val, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_LOGI(TAG, "ES8311 chip-level soft reset done (REG00=0x1F)");

    if ((e = i2s_profile_init(profile)) != ESP_OK) {
        bsp_audio_deinit();
        return e;
    }

    s_data = audio_codec_new_i2s_data(&(audio_codec_i2s_cfg_t){
        .port = BSP_I2S_PORT, .tx_handle = s_tx, .rx_handle = s_rx,
    });
    if (!s_data) { ESP_LOGE(TAG, "I2S data port creation failed"); bsp_audio_deinit(); return ESP_FAIL; }

    s_gpio = audio_codec_new_gpio();
    if (!s_gpio) { bsp_audio_deinit(); return ESP_ERR_NO_MEM; }

    s_codec = es8311_codec_new(&(es8311_codec_cfg_t){
        .ctrl_if     = s_ctrl,
        .gpio_if     = s_gpio,
        .codec_mode  = s_tx && s_rx ? ESP_CODEC_DEV_WORK_MODE_BOTH :
                       s_tx ? ESP_CODEC_DEV_WORK_MODE_DAC : ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin      = BSP_I2S_PA_CTRL,
        .pa_reverted = false,
        .master_mode = false,          // MCU I2S is master, codec is slave
        .use_mclk    = true,
        // Match the reference firmware's voltage calibration. This is not a
        // measured PA supply: change playback gain through the volume curve,
        // not by inventing a different board voltage.
        .hw_gain     = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f },
        // WARNING: must be true for mono pure-microphone recording. false makes the driver write REG44=0x58 into
        //   ADCL+DACR reference mode, and the channel read in mono is the DAC reference -> [recording is always 0].
        .no_dac_ref  = true,
    });
    if (!s_codec) { ESP_LOGE(TAG, "es8311_codec_new failed"); bsp_audio_deinit(); return ESP_FAIL; }

    s_dev = esp_codec_dev_new(&(esp_codec_dev_cfg_t){
        .dev_type = s_tx && s_rx ? ESP_CODEC_DEV_TYPE_IN_OUT :
                    s_tx ? ESP_CODEC_DEV_TYPE_OUT : ESP_CODEC_DEV_TYPE_IN,
        .codec_if = s_codec,
        .data_if  = s_data,
    });
    if (!s_dev) { ESP_LOGE(TAG, "esp_codec_dev_new failed"); bsp_audio_deinit(); return ESP_FAIL; }

    if (s_tx && bsp_audio_volume_configure(s_dev) != 0) {
        ESP_LOGE(TAG, "Output volume curve config failed");
        bsp_audio_deinit();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ES8311 ready (low-noise curve below 80%%, max +6dB)");
    return ESP_OK;
}

esp_err_t bsp_audio_init(void) {
    control_lock();
    esp_err_t result = audio_init(AUDIO_DUPLEX);
    control_unlock();
    return result;
}
esp_err_t bsp_audio_init_capture(void) {
    control_lock();
    esp_err_t result = audio_init(AUDIO_CAPTURE);
    control_unlock();
    return result;
}
esp_err_t bsp_audio_init_playback(void) {
    control_lock();
    esp_err_t result = audio_init(AUDIO_PLAYBACK);
    control_unlock();
    return result;
}
esp_err_t bsp_audio_init_feedback(void) {
    control_lock();
    esp_err_t result = s_dev ? ESP_ERR_INVALID_STATE : audio_init(AUDIO_FEEDBACK);
    if (result == ESP_OK) {
        s_before_feedback_volume = s_requested_volume;
        s_feedback_owner = true;
    }
    control_unlock();
    return result;
}

void bsp_audio_deinit(void) {
    control_lock();
    if (s_dev) {
        // delete closes even a partially opened device, but does not own these
        // separately allocated interfaces. Release them after the device.
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
    }
    s_opened = false;
    if (s_codec) { audio_codec_delete_codec_if(s_codec); s_codec = NULL; }
    if (s_data) { audio_codec_delete_data_if(s_data); s_data = NULL; }
    if (s_ctrl) { audio_codec_delete_ctrl_if(s_ctrl); s_ctrl = NULL; }
    if (s_gpio) { audio_codec_delete_gpio_if(s_gpio); s_gpio = NULL; }
    // Before the first open, channels are primed by this file, not the data
    // interface. Account for init failures as well as normal recording stops.
    if (s_tx_primed) i2s_channel_disable(s_tx);
    if (s_rx_primed) i2s_channel_disable(s_rx);
    s_tx_primed = s_rx_primed = false;
    if (s_tx) {
        i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    if (s_rx) {
        i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    s_hz = 0; s_bits = 0; s_ch = 0;
    if (s_feedback_owner) {
        s_requested_volume = s_before_feedback_volume;
        s_feedback_owner = false;
    }
    ESP_LOGI(TAG, "audio deinit, heap returned");
    control_unlock();
}

void bsp_audio_prepare_sleep(void) {
    // Do not free objects that an audio worker may still reference. Closing
    // powers down ES8311; keeping the lock prevents a late worker reopening it.
    // The caller immediately enters deep sleep and never returns to normal use.
    control_lock();
    if (s_dev) esp_codec_dev_close(s_dev);
    s_opened = false;
}

static esp_err_t set_format(uint32_t hz, uint8_t bits, uint8_t ch) {
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (s_opened && s_hz == hz && s_bits == bits && s_ch == ch) return ESP_OK;   // same format, reuse

    if (s_opened) {
        esp_codec_dev_close(s_dev);
        s_opened = false;
        // close returns the I2S channel to READY, and the open that follows disables once more internally ->
        // it would log "channel has not been enabled yet". Enable once more to make it legal.
        if (s_tx) s_tx_primed = i2s_channel_enable(s_tx) == ESP_OK;
        if (s_rx) s_rx_primed = i2s_channel_enable(s_rx) == ESP_OK;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = ch,
        .channel_mask = 0,           // aligned with the official folo-ai-passport-xiaozhi: 0 enables I2S_STD_SLOT_BOTH to drive both slots, avoiding high-frequency clock ringing from a floating single slot
        .sample_rate = hz,
        .mclk_multiple = 0,          // 0 -> the driver uses the default 256xfs for MCLK
    };
    int r = esp_codec_dev_open(s_dev, &fs);
    s_tx_primed = s_rx_primed = false; // codec/data interfaces now own channel state
    if (r != 0) {
        // esp_codec_dev_open marks input/output opened before set_fs/enable.
        // Clear that partial state or the next retry can return a false success.
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", r);
        esp_codec_dev_close(s_dev);
        return ESP_FAIL;
    }

    // WARNING: after open, [do not] manually overwrite the ES8311 clock divider registers (REG01~06):
    //   the driver has computed them exactly from the sample rate and MCLK; overwriting garbles ADC/DAC timing and recording/playback is all noise.
    //   Only restore the microphone analog PGA gain and logical volume here. close/open resets the codec output
    //   state; without restoring it, switching from 16kHz recording to 24kHz TTS may stay silent.
    if (s_rx) esp_codec_dev_set_in_gain(s_dev, 30.0f);
    const uint8_t actual_volume = bsp_audio_volume_effective(s_requested_volume, s_master_volume);
    if (s_tx && bsp_audio_volume_apply(s_dev, s_requested_volume, s_master_volume) != 0) {
        ESP_LOGE(TAG, "Failed to restore output volume: %u%%", actual_volume);
        esp_codec_dev_close(s_dev);
        return ESP_FAIL;
    }

    s_opened = true; s_hz = hz; s_bits = bits; s_ch = ch;
    ESP_LOGI(TAG, "codec opened %luHz/%ubit/%uch, output volume %u%%",
             (unsigned long)hz, bits, ch, actual_volume);
    return ESP_OK;
}

esp_err_t bsp_audio_set_format(uint32_t hz, uint8_t bits, uint8_t ch) {
    control_lock();
    esp_err_t result = set_format(hz, bits, ch);
    control_unlock();
    return result;
}

esp_err_t bsp_audio_write(const void *pcm, size_t bytes) {
    if (!s_dev || !s_opened || !s_tx) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_write(s_dev, (void *)pcm, bytes) == 0 ? ESP_OK : ESP_FAIL;
}

// Blocking read with no timeout (intentional, confirmed in the 2026-08-29 review). esp_codec_dev_read is built on
// i2s_channel_read(..., portMAX_DELAY): I2S RX is driven by the MCLK hardware clock, and as long as the channel
// is enabled it produces DMA descriptors steadily at 16kHz; "a read never returns" means the clock or DMA is already dead -
// in that state a timeout would only turn a diagnosable hang (visible on the task stack) into a string of
// ESP_FAIL and silent frames. The caller (audio_worker in audio_streamer) is a dedicated task whose priority
// is below the watchdog-feeding IDLE, so a hang will not take the system down; at session end the loop exits via the s_active flag, checked only after the last
// read returns, so the worst case waits one extra block (100ms).
esp_err_t bsp_audio_read(void *pcm, size_t bytes) {
    if (!s_dev || !s_opened || !s_rx) return ESP_ERR_INVALID_STATE;
    return esp_codec_dev_read(s_dev, pcm, bytes) == 0 ? ESP_OK : ESP_FAIL;
}

void bsp_audio_set_volume(uint8_t percent) {
    control_lock();
    if (percent > 100) percent = 100;
    s_requested_volume = percent;
    if (s_opened && s_tx && bsp_audio_volume_apply(s_dev, s_requested_volume, s_master_volume) != 0) {
        ESP_LOGW(TAG, "Failed to apply app volume");
    }
    control_unlock();
}

void bsp_audio_set_master_volume(uint8_t percent) {
    control_lock();
    if (percent > 100) percent = 100;
    s_master_volume = percent;
    if (s_opened && s_tx && bsp_audio_volume_apply(s_dev, s_requested_volume, s_master_volume) != 0) {
        ESP_LOGW(TAG, "Failed to set system volume");
    }
    ESP_LOGI(TAG, "master volume=%u%%, app=%u%%, output=%u%%", s_master_volume,
             s_requested_volume, bsp_audio_volume_effective(s_requested_volume, s_master_volume));
    control_unlock();
}

uint8_t bsp_audio_get_master_volume(void) {
    return s_master_volume;
}
