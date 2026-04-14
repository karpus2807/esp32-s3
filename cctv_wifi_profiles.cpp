#include "cctv_wifi_profiles.h"

#include <Preferences.h>

extern String g_wifiSsid;
extern String g_wifiPass;
extern bool g_wifiEnterprise;
extern String g_wifiIdentity;
extern String g_wifiEapPass;
extern bool connectWifi(uint32_t timeoutMs);

namespace {

struct Slot {
  String ssid;
  String pass;
  bool ent = false;
  String ident;
  String eap;
};

Slot s_slots[3];
uint8_t s_pref = 0;

static const char *kSsid(int i) {
  static char k[] = "wp0s";
  k[2] = (char)('0' + i);
  return k;
}
static const char *kPass(int i) {
  static char k[] = "wp0p";
  k[2] = (char)('0' + i);
  return k;
}
static const char *kEnt(int i) {
  static char k[] = "wp0e";
  k[2] = (char)('0' + i);
  return k;
}
static const char *kId(int i) {
  static char k[] = "wp0i";
  k[2] = (char)('0' + i);
  return k;
}
static const char *kEp(int i) {
  static char k[] = "wp0w";
  k[2] = (char)('0' + i);
  return k;
}

static void read_slots(Preferences &p) {
  for (int i = 0; i < 3; ++i) {
    s_slots[i].ssid = p.getString(kSsid(i), "");
    s_slots[i].pass = p.getString(kPass(i), "");
    s_slots[i].ent = p.getUChar(kEnt(i), 0) != 0;
    s_slots[i].ident = p.getString(kId(i), "");
    s_slots[i].eap = p.getString(kEp(i), "");
  }
  s_pref = p.getUChar("wpPref", 0);
  if (s_pref > 2u) {
    s_pref = 0;
  }
}

static void write_slots(Preferences &p) {
  for (int i = 0; i < 3; ++i) {
    if (s_slots[i].ssid.length() == 0) {
      p.remove(kSsid(i));
      p.remove(kPass(i));
      p.remove(kEnt(i));
      p.remove(kId(i));
      p.remove(kEp(i));
    } else {
      p.putString(kSsid(i), s_slots[i].ssid);
      p.putString(kPass(i), s_slots[i].pass);
      p.putUChar(kEnt(i), s_slots[i].ent ? 1u : 0u);
      p.putString(kId(i), s_slots[i].ident);
      p.putString(kEp(i), s_slots[i].eap);
    }
  }
  p.putUChar("wpPref", s_pref);
}

static uint8_t first_filled_slot() {
  for (uint8_t i = 0; i < 3; ++i) {
    if (s_slots[i].ssid.length() > 0) {
      return i;
    }
  }
  return 0;
}

}  // namespace

void cctv_wifi_load_profiles() {
  for (auto &sl : s_slots) {
    sl = Slot{};
  }
  s_pref = 0;

  Preferences p;
  if (!p.begin("cctv", true)) {
    return;
  }

  const bool newFmt = p.getString(kSsid(0), "").length() > 0 ||
                      p.getString(kSsid(1), "").length() > 0 ||
                      p.getString(kSsid(2), "").length() > 0;

  if (!newFmt) {
    const String legS = p.getString("wSsid", "");
    if (legS.length() > 0) {
      s_slots[0].ssid = legS;
      s_slots[0].pass = p.getString("wPass", "");
      s_slots[0].ent = p.getBool("wEnt", false);
      s_slots[0].ident = p.getString("wIdent", "");
      s_slots[0].eap = p.getString("wEapPass", "");
      s_pref = 0;
      p.end();
      if (Preferences pw; pw.begin("cctv", false)) {
        write_slots(pw);
        pw.remove("wSsid");
        pw.remove("wPass");
        pw.remove("wEnt");
        pw.remove("wIdent");
        pw.remove("wEapPass");
        pw.end();
      }
      if (!p.begin("cctv", true)) {
        return;
      }
    }
  }

  read_slots(p);
  p.end();
}

void cctv_wifi_commit_all_slots_to_nvs() {
  Preferences p;
  if (!p.begin("cctv", false)) {
    return;
  }
  write_slots(p);
  p.end();
}

void cctv_wifi_clear_all_slots_nvs() {
  for (int i = 0; i < 3; ++i) {
    s_slots[i] = Slot{};
  }
  s_pref = 0;
  Preferences p;
  if (!p.begin("cctv", false)) {
    return;
  }
  for (int i = 0; i < 3; ++i) {
    p.remove(kSsid(i));
    p.remove(kPass(i));
    p.remove(kEnt(i));
    p.remove(kId(i));
    p.remove(kEp(i));
  }
  p.remove("wpPref");
  p.remove("wSsid");
  p.remove("wPass");
  p.remove("wEnt");
  p.remove("wIdent");
  p.remove("wEapPass");
  p.end();
}

bool cctv_wifi_any_profile_configured() {
  for (int i = 0; i < 3; ++i) {
    if (s_slots[i].ssid.length() > 0) {
      return true;
    }
  }
  return false;
}

void cctv_wifi_apply_slot_to_globals(uint8_t slot) {
  if (slot > 2u) {
    slot = 0;
  }
  g_wifiSsid = s_slots[slot].ssid;
  g_wifiPass = s_slots[slot].pass;
  g_wifiEnterprise = s_slots[slot].ent;
  g_wifiIdentity = s_slots[slot].ident;
  g_wifiEapPass = s_slots[slot].eap;
}

void cctv_wifi_apply_preferred_or_first_globals() {
  if (s_slots[s_pref].ssid.length() > 0) {
    cctv_wifi_apply_slot_to_globals(s_pref);
    return;
  }
  for (uint8_t i = 0; i < 3; ++i) {
    if (s_slots[i].ssid.length() > 0) {
      cctv_wifi_apply_slot_to_globals(i);
      return;
    }
  }
  g_wifiSsid = "";
  g_wifiPass = "";
  g_wifiEnterprise = false;
  g_wifiIdentity = "";
  g_wifiEapPass = "";
}

void cctv_wifi_save_globals_into_slot(uint8_t slot) {
  if (slot > 2u) {
    return;
  }
  s_slots[slot].ssid = g_wifiSsid;
  s_slots[slot].pass = g_wifiPass;
  s_slots[slot].ent = g_wifiEnterprise;
  s_slots[slot].ident = g_wifiIdentity;
  s_slots[slot].eap = g_wifiEapPass;
  cctv_wifi_commit_all_slots_to_nvs();
}

void cctv_wifi_save_globals_into_best_slot() {
  const String want = g_wifiSsid;
  if (want.length() == 0) {
    return;
  }
  for (uint8_t i = 0; i < 3; ++i) {
    if (s_slots[i].ssid == want) {
      cctv_wifi_save_globals_into_slot(i);
      return;
    }
  }
  for (uint8_t i = 0; i < 3; ++i) {
    if (s_slots[i].ssid.length() == 0) {
      cctv_wifi_save_globals_into_slot(i);
      return;
    }
  }
  cctv_wifi_save_globals_into_slot(2);
}

void cctv_wifi_delete_slot(uint8_t slot) {
  if (slot > 2u) {
    return;
  }
  s_slots[slot] = Slot{};
  if (s_pref == slot) {
    s_pref = first_filled_slot();
  }
  cctv_wifi_commit_all_slots_to_nvs();
}

uint8_t cctv_wifi_preferred_slot() {
  return s_pref;
}

void cctv_wifi_set_preferred_slot(uint8_t slot) {
  if (slot < 3u) {
    s_pref = slot;
  }
  cctv_wifi_commit_all_slots_to_nvs();
}

bool cctv_wifi_try_connect_profiles(uint32_t timeout_per_slot_ms) {
  uint8_t order[3];
  uint8_t n = 0;
  order[n++] = s_pref;
  for (uint8_t i = 0; i < 3; ++i) {
    if (i != s_pref) {
      order[n++] = i;
    }
  }
  for (uint8_t k = 0; k < 3; ++k) {
    const uint8_t idx = order[k];
    if (s_slots[idx].ssid.length() == 0) {
      continue;
    }
    cctv_wifi_apply_slot_to_globals(idx);
    if (connectWifi(timeout_per_slot_ms)) {
      s_pref = idx;
      cctv_wifi_commit_all_slots_to_nvs();
      return true;
    }
  }
  return false;
}

static void json_escape_ssid(String &out, const String &s) {
  for (size_t p = 0; p < s.length(); ++p) {
    const char c = s[p];
    if (c == '"' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
}

void cctv_wifi_print_profiles(Print &out) {
  out.printf("[WiFi] Preferred slot: %u\n", (unsigned)s_pref);
  for (int i = 0; i < 3; ++i) {
    if (s_slots[i].ssid.length() == 0) {
      out.printf("  [%d] (empty)\n", i);
    } else {
      out.printf(
          "  [%d] SSID=\"%s\"  enterprise=%s  pass=%s  eap=%s\n",
          i,
          s_slots[i].ssid.c_str(),
          s_slots[i].ent ? "yes" : "no",
          s_slots[i].pass.length() ? "(saved)" : "(none)",
          s_slots[i].eap.length() ? "(saved)" : "(none)");
    }
  }
}

String cctv_wifi_profiles_json_array() {
  String j = "[";
  for (int i = 0; i < 3; ++i) {
    if (i) {
      j += ',';
    }
    j += "{\"slot\":";
    j += String(i);
    j += ",\"ssid\":\"";
    json_escape_ssid(j, s_slots[i].ssid);
    j += "\",\"enterprise\":";
    j += s_slots[i].ent ? "true" : "false";
    j += ",\"hasPass\":";
    j += (s_slots[i].pass.length() > 0) ? "true" : "false";
    j += ",\"hasEap\":";
    j += (s_slots[i].eap.length() > 0) ? "true" : "false";
    j += ",\"pref\":";
    j += (s_pref == (unsigned)i) ? "true" : "false";
    j += "}";
  }
  j += "]";
  return j;
}
