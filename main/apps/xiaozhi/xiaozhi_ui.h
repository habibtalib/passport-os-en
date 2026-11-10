#pragma once

#include "apps/xiaozhi/xiaozhi_protocol.h"
#include <string>

namespace xiaozhi {

// The UI layer may only be called from the "control task"; every entry point re-validates the widget pointer INSIDE the LVGL lock.
//
// The old implementation put `if (!s_status_label || !lock())` before taking the lock: ui_hide() deletes the widgets
// while holding the lock and nulls the pointer, while a network task could be preempted after the check passed but before
// taking the lock -- then it called lv_label_set_text() on a freed lv_obj, one source of the
// occasional crash during conversation. Now validation and use happen under the same lock, so this window no longer exists.

void ui_init();
void ui_show();
void ui_hide();

void ui_set_state(State state);                       // Top status capsule
void ui_set_status_text(const std::string &text);      // Custom status text (activation code / error)
void ui_set_user_text(const std::string &text);        // What the user said (replace)
void ui_set_ai_text(const std::string &text);          // Xiaozhi reply (replace)
void ui_append_ai_text(const std::string &text);       // Xiaozhi reply (append)
void ui_set_hint(const std::string &text);             // Bottom key hint
void ui_clear_text();
void ui_set_volume(unsigned percent);

} // namespace xiaozhi
