# RS41 sensor boom (PTU) support

The Vaisala RS41 carries three ring-oscillator channels on its sensor boom:

- a PT main-air temperature sensor
- a capacitive humidity sensor
- a PT boom-temperature sensor (mounted on the humidity cap, used for physical RH correction)

Converting the raw cycle counts to Celsius / %RH requires per-sonde factory calibration stored in the STM32 flash. **Flashing RS41ng erases this data permanently**, so it must be captured beforehand.

## Procedure

### 1. Capture the calibration with `radiosonde_auto_rx` — *before* flashing

Power the factory-firmware sonde and let `radiosonde_auto_rx` decode it for a few minutes. The full calibration is spread across 51 subframes that the sonde cycles through every ~50 s; give it **at least 2–3 minutes** on a clean signal to be sure every subframe has been received at least once.

`auto_rx` drops the captured subframe blob in its `log/` directory as `*_subframe.bin` (816 bytes, identical layout to the `calibytes[]` array inside `rs41mod.c`).

### 2. Generate `src/sensor_cal.h`

```
python3 tools/extract_rs41_cal.py ~/radiosonde_auto_rx/log/YYYYMMDD-HHMMSS_<serial>_RS41-NG_<freq>_subframe.bin
```

This validates the CRC16 stored in the first two bytes, extracts the per-sonde cal floats, and writes `src/sensor_cal.h`. `sensor_cal.h` is gitignored — it is user data, not source — so swapping sondes is just "re-run the extractor, rebuild, re-flash."

Handy flags:

| flag | effect |
|---|---|
| `--stdout` | print to stdout instead of writing (good for diffs) |
| `-o PATH`  | write somewhere other than `src/sensor_cal.h` |
| `--force`  | overwrite an existing `sensor_cal.h`, or emit despite CRC failure (stamped as such in the header comment) |

If the extractor warns that the Tier-2 arrays (`MTXH`, `CORHP`, `CORHT`) are all `0xFF`, the `auto_rx` capture ran too short to see subframes 0x07–0x11 / 0x2A–0x2E. Either re-capture for longer, or build with `SENSOR_BOOM_RH_MODEL_PHYSICAL=false`.

### 3. Enable the driver in `config.h`

```c
#define SENSOR_BOOM_ENABLE                 true
#define SENSOR_BOOM_RH_MODEL_PHYSICAL      false   // "true" once you have Tier-2 cal
```

Then rebuild and flash as usual. The sensor readings are published through the existing `telemetry_data` struct, so Horus Binary v1/v2, RTTY, and APRS formats all pick them up without further configuration.

## Calibration contents

### Tier 1 — empirical RH path (always required when the driver is enabled)

| macro | source | meaning |
|---|---|---|
| `SENSOR_BOOM_RF1`, `SENSOR_BOOM_RF2` | `calibytes[61]`, `[65]` | reference resistors (~750 Ω, ~1100 Ω) |
| `SENSOR_BOOM_CO1[3]`                 | `calibytes[77..88]`     | main-T polynomial (Callendar-Van-Dusen-like) |
| `SENSOR_BOOM_CALT1[3]`               | `calibytes[89..100]`    | main-T per-sonde trim |
| `SENSOR_BOOM_CALH0`                  | `calibytes[117]`        | dry-cap reference |
| `SENSOR_BOOM_CO2[3]`                 | `calibytes[293..304]`   | boom-T polynomial |
| `SENSOR_BOOM_CALT2[3]`               | `calibytes[305..316]`   | boom-T per-sonde trim |

### Tier 2 — physical RH path (`get_RH2adv`)

| macro | source | meaning |
|---|---|---|
| `SENSOR_BOOM_CF1`, `SENSOR_BOOM_CF2` | `calibytes[69]`, `[73]` | reference caps |
| `SENSOR_BOOM_CALH1`                  | `calibytes[121]`        | cap scale |
| `SENSOR_BOOM_MTXH[42]`               | `calibytes[125..292]`   | 7×6 RH polynomial (Cp, TH) |
| `SENSOR_BOOM_CORHP[3]`               | `calibytes[678..689]`   | pressure-correction coefficients |
| `SENSOR_BOOM_CORHT[12]`              | `calibytes[698..745]`   | RH×T cross-terms |

All fields are little-endian IEEE-754 float32. Credit to the `radiosonde_auto_rx` / `rs1729` reverse-engineering work for the field layout.
