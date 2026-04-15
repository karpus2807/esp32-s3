#pragma once

#include <Arduino.h>
#include <stdint.h>

// Up to 3 saved STA profiles (NVS). Failover: try preferred slot first, then others.
// Legacy keys wSsid/wPass/... are migrated once into slot 0 on load.

void cctv_wifi_load_profiles();

// Persist all slots + preferred index (call after editing globals then assigning to a slot).
void cctv_wifi_commit_all_slots_to_nvs();

// Copy globals → slot that matches SSID, else first empty, else slot 0; then commit.
void cctv_wifi_save_globals_into_best_slot();

void cctv_wifi_clear_all_slots_nvs();

bool cctv_wifi_any_profile_configured();

// Apply slot to g_wifi* (no NVS write).
void cctv_wifi_apply_slot_to_globals(uint8_t slot);

// Set g_wifi* from preferred slot if set, else first non-empty slot, else clear globals.
void cctv_wifi_apply_preferred_or_first_globals();

// Try profiles in failover order; on success sets preferred index + globals. Uses connectWifi().
bool cctv_wifi_try_connect_profiles(uint32_t timeout_per_slot_ms);

uint8_t cctv_wifi_preferred_slot();
void cctv_wifi_set_preferred_slot(uint8_t slot);

void cctv_wifi_delete_slot(uint8_t slot);
bool cctv_wifi_slot_has_profile(uint8_t slot);

// For GET /wifi JSON extension: [{slot,ssid,enterprise,hasPass,hasEap},...]
String cctv_wifi_profiles_json_array();

// Serial / debug: print saved slots (passwords masked).
void cctv_wifi_print_profiles(Print &out);

// Slot editor: copy from globals into slot then commit (web explicit slot).
void cctv_wifi_save_globals_into_slot(uint8_t slot);
