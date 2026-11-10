#pragma once

// NVS config read/write for the Xiaozhi app (namespace "xiaozhi").
//
// Split into its own small module because ws_url / token / version / client_id now have 3 writers
// (OTA activation, serial config command, first-time UUID generation); scattered nvs_open/nvs_close calls easily miss
// an nvs_close and leak a handle, so centralizing them leaves only one place to audit.

// Plain C++ interface (uses std::string, so no extern "C" linkage). Only for inclusion by the
// .cc files inside the Xiaozhi app.
#include <string>

// Xiaozhi config namespace
extern const char *const kNsXiaozhi;
// Namespace of the official xiaozhi-esp32 websocket config (OTA delivers url/token/version);
// fall back to it if the local xiaozhi namespace is missing, so the official backend config can be reused directly.
extern const char *const kNsWebsocket;

// Read a string; returns def if missing or on failure.
std::string store_get_string(const char *ns, const char *key, const char *def);

// Read an integer; returns def if missing or on failure.
int store_get_int(const char *ns, const char *key, int def);

// Write a string. Returns whether it succeeded.
bool store_set_string(const char *ns, const char *key, const std::string &value);

// Write an integer. Returns whether it succeeded.
bool store_set_int(const char *ns, const char *key, int value);

// Erase a key
bool store_erase(const char *ns, const char *key);
