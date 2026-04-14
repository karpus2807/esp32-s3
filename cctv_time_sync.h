#pragma once

#include <stddef.h>

// GET url (JSON or plain). Looks for "unixtime": <seconds> in the body and calls settimeofday.
// Call only after WiFi is connected and configTzTime() has been run (timezone for strftime/OSD).
bool cctvHttpTimeSync(const char *url);

// Call periodically (~250 ms) from a lightweight task; replaces Serial-only NTP polling.
void cctv_poll_ntp_housekeeping(void);

// True if localtime looks like a real calendar year (not 1970 / unset).
bool cctv_wall_clock_sane(void);

// When internet just became reachable: restart SNTP/HTTP sync (force=true bypasses throttle).
void cctv_time_kick_sync_if_needed(bool force);

// Save current Unix epoch to NVS after a good sync (diagnostics; cannot recover wall time
// across full power loss without an external RTC + battery).
void cctv_time_nvs_snapshot_wall(void);
