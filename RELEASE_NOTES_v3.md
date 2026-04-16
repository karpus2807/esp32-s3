# Release v3

## Highlights
- **MQTT toggle (Web UI)**: Enable/disable MQTT from the dashboard; stored in NVS (`iot/mqEn`). Detection/sensors keep running when MQTT is off.
- **Camera motion (no PIR)**: Frame-diff motion with smoothing + adaptive effective threshold; diagnostics exposed via `/api/iot`.
- **LAN / time sync**: Ethernet DHCP success now kicks time sync; DNS fallback applied via ETH netif to improve NTP/HTTP hostname resolution on some LANs.
- **Recording**: Timestamp OSD applied consistently (no overlay flicker); recording waits for sane wall clock; default record FPS tuned for smoother AVI on ESP32-S3.
- **Storage**: “Wipe All” is recursive under `/captures` and safer during active recording; optional deep-wipe from SD root; remount refreshes free-space reporting.
- **Alert levels**: `alert_level` is now **0..3** using alert/warn/critical thresholds; UI shows **OK / ALERT / WARNING / CRITICAL**.

## Build / flash (mandatory for N16R8)
Use the FQBN from `FLASHING_INSTRUCTIONS.md`:
`esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi,FlashSize=16M`

## Known limitations
- True “full SD format” (recreate FAT) is not exposed via Arduino `SD_MMC.format()`; use **deep wipe** for “erase everything” intent (filesystem metadata may still show small used space).
