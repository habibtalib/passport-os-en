#pragma once

// Clausium background alerts: one small low-priority task polls the Mac's
// /passport/v1/alert endpoint and raises one chime + banner per new event,
// whatever app is in front. The foreground Clausium app feeds the same
// tracker, so an event never chimes twice.

#include <stdbool.h>
#include <stddef.h>
#include "apps/app_base.h"
#include "clausium_config.h"
#include "clausium_feed.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLAUSIUM_ALERT_TEXT_MAX 80

// Boot: start the poller when Clausium is configured and alerts are on.
// `defer` (boot went straight into a heavy app) postpones the task until a
// non-heavy app is in front.
void clausium_alert_init(bool defer);
// After the phone page saves settings or Settings toggles alerts.
void clausium_alert_reconfigure(void);
// Called by the Clausium app on start: baseline afresh when the background
// poller was not keeping the tracker current.
void clausium_alert_app_started(void);
// One GET of /passport/v1/alert + apply. Returns false on any error.
bool clausium_alert_poll(const clausium_config_t *config);
// Applies one alert message. Returns true when a new alert was raised.
bool clausium_alert_apply(const clausium_alert_msg_t *msg);
// 1 Hz from the system monitor: wake, 8 s banner, chime.
void clausium_alert_service_tick(void);
// Apps that own the audio codec get the banner without a chime.
bool clausium_alert_sound_allowed(passport_app_id_t id);
// Current alert text (for logs and tests); empty when none was raised yet.
void clausium_alert_text(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
