# ESP32-S3 N16R8 Flashing Instructions (Mandatory)

Use this guide whenever flashing this project to the target device:

- Module: `ESP32-S3-WROOM-1U-N16R8`
- Flash: `16MB`
- PSRAM: `8MB OPI` (must be enabled as `PSRAM=opi`)

If you flash with default board settings (`PSRAM=disabled`), firmware behavior becomes unstable (PSRAM shows 0, memory pressure rises, Ethernet/W5500 errors increase).

## Required FQBN

Always use this FQBN:

`esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi,FlashSize=16M`

## Compile Command

```bash
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi,FlashSize=16M \
  --warnings default \
  .
```

## Upload Command

```bash
arduino-cli upload \
  -p /dev/ttyACM0 \
  --fqbn esp32:esp32:esp32s3:CDCOnBoot=cdc,PartitionScheme=huge_app,PSRAM=opi,FlashSize=16M \
  .
```

## Safe Flash Workflow (Agents)

1. Close any serial monitor using `/dev/ttyACM0`.
2. (Optional) Free port before flashing:
   - `fuser -k /dev/ttyACM0`
3. Run compile command above.
4. Run upload command above.
5. Verify boot log after reset.

## Post-Flash Validation

After boot, confirm:

- Device is detected as ESP32-S3.
- PSRAM is available (do not rely only on `ESP.getPsramSize()`; use heap-caps based PSRAM stats).
- Web UI and network functions respond.

## Important Notes

- Do not use board defaults for this project.
- Do not flash with `PSRAM=disabled`.
- Keep using the exact FQBN from this file for all future flashes (manual or agent-driven).

