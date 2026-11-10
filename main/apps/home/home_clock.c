#include "apps/home/home_clock.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"

#include <stdio.h>
#include <time.h>

#include "apps/voice/time_sync.h"

static const char *TAG = "home_clock";

#define CLOCK_EPOCH_MIN 1700000000LL /* 2023-11; anything earlier counts as unsynced */

static void on_time_sync(struct timeval *tv) {
    if (tv && tv->tv_sec >= CLOCK_EPOCH_MIN) time_sync_set_epoch(tv->tv_sec);
}

void home_clock_start(void) {
    if (esp_sntp_enabled()) return;
    // Single server (LWIP_SNTP_MAX_SERVERS=1); the Aliyun node has the best reachability in China;
    // on resolve failure SNTP itself retries at its interval, no outer retry task needed.
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(on_time_sync);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
}

void home_clock_stop(void) {
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
        ESP_LOGI(TAG, "SNTP stopped");
    }
}

bool home_clock_valid(void) {
    time_t now = time(NULL);
    return now > CLOCK_EPOCH_MIN;
}

void home_clock_format_time(char *buf, size_t cap) {
    if (!buf || cap < 6) return;
    if (!home_clock_valid()) {
        snprintf(buf, cap, "--:--");
        return;
    }
    time_t t = time(NULL) + (time_t)time_sync_tz_offset_sec();
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, cap, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void home_clock_format_date(char *buf, size_t cap) {
    if (!buf || cap < 16) return;
    if (!home_clock_valid()) {
        snprintf(buf, cap, "----");
        return;
    }
    time_t t = time(NULL) + (time_t)time_sync_tz_offset_sec();
    struct tm tm;
    gmtime_r(&t, &tm);
    static const char *kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(buf, cap, "%s %02d", kMon[tm.tm_mon % 12], tm.tm_mday);
}

void home_clock_format_weekday(char *buf, size_t cap) {
    static const char *kWeek[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    if (!buf || cap < 8) return;
    if (!home_clock_valid()) {
        snprintf(buf, cap, "----");
        return;
    }
    time_t t = time(NULL) + (time_t)time_sync_tz_offset_sec();
    struct tm tm;
    gmtime_r(&t, &tm);
    snprintf(buf, cap, "%s", kWeek[tm.tm_wday % 7]);
}
