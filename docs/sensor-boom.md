# The RS41 sensor boom: how it works and how RS41ng reads it

The Vaisala RS41 "sensor boom" (also called the sensor stalk or TSU) is the
flexible arm sticking out of the top of the sonde. It carries the sensors used
for the meteorological measurements:

- a **platinum resistance thermometer** (PT1000-style, but not exactly PT1000)
  for the main air temperature,
- a **capacitive humidity sensor**,
- a **second platinum resistance thermometer** bonded to the humidity sensor,
  used both to control the humidity sensor's de-icing heater and to
  temperature-compensate the humidity reading,
- a **resistive heater** for the humidity sensor.

There is no interface chip on the boom — the bare sensor elements connect
through a flex cable directly to analog circuitry on the main PCB, and
everything is done in firmware. That is what makes it possible for third-party
firmware like RS41ng to read the original sensors.

This document describes how the boom interface works at the hardware level,
where the calibration math and coefficients come from, and how RS41ng
implements it.

Sources used throughout:

- **The RS41 hardware documentation** ([bazjo's schematic and logic-analyzer
  captures](https://github.com/bazjo/RS41_Hardware)) — the measurement circuit,
  multiplexer truth table and STM32 pin map.
- **rs1729/RS** (`rs41/rs41ptu.c`, `demod/mod/rs41mod.c`) — the reverse
  engineering of Vaisala's own calibration math, reproduced by every RS41
  telemetry decoder.
- **radiosonde_auto_rx** (`auto_rx/utils/rs41cal.py`) — the byte-level layout
  of the calibration ("subframe") data, following the field names used by
  einergehtnochrein's ra-firmware.
- DF8OE's hardware investigation notes (`docs/infos_for_sensors.txt`).

## The measurement principle: one oscillator, seven inputs

The RS41 does not use an ADC to read the sensors. Instead, the main PCB
contains a **relaxation oscillator** whose output frequency depends on the
resistance (or capacitance) connected to it. The sensor elements and a set of
precision on-board references are switched into this oscillator one at a time
through analog switches, and the MCU measures the resulting frequency on a
timer input-capture pin.

Two properties make this arrangement remarkably accurate:

1. The oscillator **period is very nearly a linear function** of the connected
   resistance/capacitance over the operating range.
2. Every measurement cycle also measures **two known references** on the same
   oscillator (750 Ω and 1100 Ω for the resistive channels; 0 pF and 47 pF for
   the capacitive channels). The unknown sensor value is obtained by linear
   interpolation between the references, so the absolute oscillator frequency,
   timer clock accuracy, temperature drift of the oscillator, and any constant
   scale factor all cancel out. Only short-term stability matters.

There are two independent oscillator input rails, each activated by its own
bias/pull-up MOSFET:

- the **temperature rail** (`PULLUP_TM`) for the resistive channels
  (references, main temperature, humidity-sensor temperature), switched
  through four SPST analog switches, and
- the **humidity rail** (`PULLUP_HYG`) for the capacitive channels (humidity
  sensor, capacitance references), switched through three SPDT analog
  switches.

Both rails feed the same output pin, `MEAS_OUT` (PA1), which is a square wave
in the tens of kilohertz.

## Pin map

The two RS41 hardware generations use the same circuit with different GPIO
assignments. "Classic" is the original STM32F100-based board (RSM412 and
earlier, sondes with R/S/T serials); "RSM4x4" is the newer STM32L412-based
board (RSM414/RSM424, "RS41-NG" variant, X/V/W serials).

| Signal | Function | Classic (F100) | RSM4x4 (L412) |
|---|---|---|---|
| `MEAS_OUT` | Oscillator output → timer input capture | PA1 (TIM2_CH2) | PA1 (TIM2_CH2, AF1) |
| `PULLUP_TM` | Temperature-rail oscillator bias | PB12 | PB12 |
| `PULLUP_HYG` | Humidity-rail oscillator bias | PA2 | PA2 |
| `SPST1` | 750 Ω reference resistor | PB6 | PB3 |
| `SPST2` | 1100 Ω reference resistor | PA3 | PA3 |
| `SPST3` | Humidity-sensor temperature (PT1000 on boom) | PC14 | PC14 |
| `SPST4` | Main air temperature (PT1000 on boom) | PC15 | PC15 |
| `SPDT1` | Humidity sensor (capacitive, on boom) | PB3 | PC10 |
| `SPDT2` | 47 pF reference capacitor | PB4 | PC11 |
| `SPDT3` | 0 pF reference ("empty" input) | PB5 | PC12 |
| `HEAT_HUM1/2` | Humidity sensor heater drive | PA7, PB9 | PA7, PB8 |

Notes:

- On the classic board, `SPST1` (PB6), `SPDT1` (PB3) and `SPDT2` (PB4) sit on
  JTAG pins (PB3 = JTDO, PB4 = NJTRST). The firmware must remap
  SWJ to "SWD only, no JTAG" before driving them as GPIO. SWD debugging on
  PA13/PA14 is unaffected.
- All switch and bias pins are push-pull outputs, active high, and must idle
  low: driving them low powers the oscillator down and disconnects all
  channels.
- The heater pins are not needed for measurements and RS41ng does not use
  them (see "What RS41ng does not implement" below).

## Measurement channels

The channels are numbered 1–7. Selecting a channel means: raise the rail bias
pin, raise that channel's switch pin, and drive the other rail's bias pin low.

| Ch | Switch | Rail | Connects | Typical frequency |
|---|---|---|---|---|
| 1 | `SPST1` | TM | 750 Ω reference | ≈ 90 kHz |
| 2 | `SPST2` | TM | 1100 Ω reference | ≈ 62 kHz |
| 3 | `SPST3` | TM | humidity-sensor PT1000 | R ≈ 750–1300 Ω |
| 4 | `SPST4` | TM | main-temperature PT1000 | R ≈ 750–1300 Ω |
| 5 | `SPDT1` | HYG | humidity capacitor | C ≈ 42–55 pF |
| 6 | `SPDT2` | HYG | 47 pF reference | (lower frequency) |
| 7 | `SPDT3` | HYG | 0 pF reference | (highest frequency) |

The oscillator frequency falls as resistance/capacitance rises; equivalently
the **period rises linearly** with R (or C). A full measurement cycle reads
all seven channels; the references (1, 2, 6, 7) are re-measured every cycle so
the ratiometric math always uses fresh values.

## How a channel is measured

To measure one channel, RS41ng does, in order:

1. **Select the channel** and wait **18 ms** for the oscillator to settle.
2. **Configure TIM2** for input capture on channel 2 (PA1): prescaler 0 (full
   timer clock), capture on every rising edge. On the F100 the timer is
   16-bit; on the L412 it is 32-bit.
3. **Disable all interrupts** (`__disable_irq()`). Any ISR that lands between
   captures adds latency jitter. (On the F100, an idle UART RX pin has also been
   observed to inject edge noise into the capture.)
4. Busy-wait for the **first rising edge**, then capture **2400 consecutive
   periods**, accumulating the tick deltas between captures
   (`totalTicks += current - previous`, with 16-bit wraparound arithmetic on
   the F100). A poll-count timeout (~10⁶ iterations per edge) aborts the
   measurement if the channel is dead (boom disconnected), yielding 0.
5. **Re-enable interrupts**, stop the timer, deselect the channel.
6. Compute the true frequency:

   `f = timer_clock × 2400 / totalTicks`

At ~60–90 kHz the capture window is 26–40 ms per channel, so a full 7-channel
cycle costs roughly **130 ms of interrupts-off time in ~35 ms chunks**, plus
7 × 18 ms of settling, around 350–450 ms in total.

The measured frequency is the reciprocal of what the calibration math wants
(Vaisala's own counts are proportional to *period*, i.e. to R and C), so the
conversion functions feed `1/f` into the interpolation. Because the reference
channels are treated identically, any constant scale factor cancels.

## From frequencies to physical values (the factory calibration)

Every RS41 transmits its per-sonde factory calibration in flight (one 16-byte
"subframe" block per telemetry frame, 51 blocks total). rs1729's decoders
reconstructed the math that Vaisala's ground software applies; RS41ng evaluates
that same factory calibration on board, and uses it as its only calibration
path.

In the formulas below `m = 1/f` (a value proportional to the oscillator
period) and `m1`, `m2` are the same quantity for the low and high reference
channel of the rail in question.

### Temperature (both PT1000 channels)

With reference resistors `Rf1 = 750 Ω` and `Rf2 = 1100 Ω`:

```
g  = (m2 - m1) / (Rf2 - Rf1)          # oscillator gain [period per ohm]
Rb = (m1·Rf2 - m2·Rf1) / (m2 - m1)    # offset resistance
Rc = m/g - Rb                         # raw sensor resistance
R  = Rc · calT
T  = (taylorT0 + taylorT1·R + taylorT2·R² + polyT0) · (1 + polyT1)   [°C]
```

The main temperature uses `calT`/`polyT` with `taylorT`; the humidity-sensor
temperature (`TH`) uses its own `calTU`/`polyTrh` with `taylorTU`.

### Relative humidity

With reference capacitors `Cf1 = 0 pF` and `Cf2 = 47 pF`:

```
cfh = (m - m1) / (m2 - m1)                 # fractional position between refs
C   = Cf1 + (Cf2 - Cf1) · cfh              # sensor capacitance [pF]
Cp  = (C / calibU0 - 1) · calibU1          # normalized capacitance
```

`Cp` and the normalized humidity-sensor temperature
`Trh = (TH - 20) / 180` index a 7×6 polynomial surface (`matrixU`):

```
RH_raw = Σⱼ Σₖ  Cpʲ · Trhᵏ · matrixU[j][k]     (j = 0..6, k = 0..5)
```

Two corrections follow:

1. an empirical low-temperature correction (applied when no pressure sensor
   data is available): `RH_raw += (T + 40) / 12` for air temperature
   `T < −40 °C`;
2. conversion from RH-at-sensor-temperature to RH-at-air-temperature via the
   ratio of saturation vapor pressures (Hyland–Wexler equation):
   `RH = RH_raw · psat(TH) / psat(T)`, clamped to 0–100 %.

This matters because the humidity sensor is heated a few degrees above
ambient (by design, and more when de-icing): RH is defined relative to
saturation at *air* temperature, so the reading at the warmer sensor must be
scaled up by the vapor-pressure ratio.

(The full Vaisala chain also includes a pressure-dependent correction using
the `vectorBp`/`matrixBt` coefficients. RS41ng omits it: it requires a pressure
measurement, and its effect is small in the troposphere.)

### Which coefficients are per-sonde?

A survey of 149 distinct sondes (the subframe dumps in `cal/`, mostly RS41-NG
with some RS41-SG/SGP) shows the coefficient set splits cleanly:

**Identical on every sonde surveyed** (constants of the sensor design, baked
into RS41ng):

| Field | Value |
|---|---|
| `refResistorLow/High` | 750.0 / 1100.0 Ω |
| `refCapLow/High` | 0.0 / 47.0 pF |
| `taylorT[0..2]` = `taylorTU[0..2]` | −243.9108, 0.187654, 8.2e-6 |
| `polyT[2..5]`, `polyTrh[2..5]` | 0 |
| `matrixU[7][6]` | 42-entry humidity surface (see `boom_handler.c`) |

**Per-sonde** (the actual factory calibration — 8 floats):

| Field | Meaning | Fleet spread (1σ) |
|---|---|---|
| `calT` | main temperature resistance gain | 1.083 ± 0.051 |
| `polyT[0]` | main temperature offset | −0.13 ± 0.08 °C-scale |
| `polyT[1]` | main temperature scale trim | 0.007 ± 0.003 |
| `calTU` | RH-sensor temperature resistance gain | 1.317 ± 0.045 |
| `polyTrh[0]` | RH-sensor temperature offset | −0.21 ± 0.14 |
| `polyTrh[1]` | RH-sensor temperature scale trim | 0.010 ± 0.003 |
| `calibU[0]` | humidity capacitance normalization | 43.26 ± 0.83 pF |
| `calibU[1]` | humidity capacitance scale | 5.067 ± 0.025 |

The gains matter: a 1σ error in `calT` shifts the computed temperature by
several °C, so **using the sonde's own coefficients is strongly
recommended**. RS41ng ships fleet-average defaults so the boom produces sane
readings out of the box, but for real flights extract the sonde's own values
(next section).

## Getting the calibration data for your sonde

The subframe is broadcast by the sonde while running the **original Vaisala
firmware**, so capture it *before* flashing RS41ng:

- **radiosonde_auto_rx** writes `*_subframe.bin` files (816 bytes: 800 bytes
  of calibration + 16 bytes of runtime data) into its log directory when
  `save_cal_data` is enabled. The files in this repository's `cal/` directory
  are exactly these.
- **SondeHub** archives the subframe for any sonde that was received
  telemetry-complete: `https://api.v2.sondehub.org/sonde/<serial>` — frames
  containing an `rs41_subframe` field hold the base64-encoded blob
  (`radiosonde_auto_rx/auto_rx/utils/rs41cal.py` automates this).

Relevant byte offsets within the 800-byte calibration image (little-endian
IEEE-754 floats; from ra-firmware via `rs41cal.py`):

| Offset | Field |
|---|---|
| 0x00D | serial (8 chars) |
| 0x03D / 0x041 | refResistorLow / High |
| 0x045 / 0x049 | refCapLow / High |
| 0x04D | taylorT[3] |
| 0x059 | calT |
| 0x05D | polyT[6] |
| 0x075 | calibU[2] |
| 0x07D | matrixU[42] |
| 0x125 | taylorTU[3] |
| 0x131 | calTU |
| 0x135 | polyTrh[6] |
| 0x218 | variant string (e.g. "RS41-NG") |

There are three ways to get the coefficients into a build:

**Automatically, in `config.yaml`-based builds** — point the config at the
subframe dump and the config generator extracts the coefficients at build
time (the path is relative to the YAML file):

```yaml
sensors:
  boom_enable: true
  boom_calibration_file: cal/20260702-110653_X4643493_RS41-NG_404801_subframe.bin
```

Any `boom_cal_*` value set explicitly in the YAML overrides the file (with a
warning), and the generator warns if the supposedly-universal constants in
the file differ from the ones compiled into the firmware.
`boom_calibration_file` is a build-time-only key: it is resolved by
`scripts/generate_config.ts` and never appears in the generated header, and
the web configurator ignores it (a browser cannot read local paths).

**Automatically, in `config.h`-based builds** — generate a supplemental
calibration header (no dependencies beyond Python 3):

```
python3 scripts/extract_boom_calibration.py <cal/file_subframe.bin | serial>
```

This writes `src/config_boom_cal.h`, which `config.h` includes automatically
when present (via `__has_include`). Its values override the fleet-average
defaults and set `SENSOR_BOOM_ENABLE true`; delete the file to revert. The
header is gitignored, and it does not apply to `config.yaml`-based builds
(the generated config replaces `config.h` entirely — use
`boom_calibration_file` there instead).

**Manually** — run the TypeScript extraction script and paste its output:

```
bun run scripts/extract_boom_calibration.ts <cal/file_subframe.bin | serial>
```

It prints the eight `SENSOR_BOOM_CAL_*` defines for `config.h` (and the
matching `sensors:` YAML block for `config.yaml`).

All three paths perform the same universal-constant check (warning if the
sonde's subframe disagrees with the constants compiled into the firmware),
and both scripts can fetch the subframe from SondeHub when given a serial
number instead of a file.

## The RS41ng implementation

The interface is split in the usual RS41ng driver/handler pattern:

- [`src/drivers/boom/boom.c`](../src/drivers/boom/boom.c) — the hardware
  layer: channel selection (analog switches + rail bias pins from `gpio.h`)
  and frequency measurement (TIM2 CH2 input capture on PA1, both MCU families).
- [`src/boom_handler.c`](../src/boom_handler.c) — the measurement cycle and
  the factory calibration math; fills `temperature_celsius_100` and
  `humidity_percentage_100` in the telemetry struct, from which the values
  flow into Horus V2/V3, APRS weather reports and message templates like any
  external sensor.

Hardware-sharing notes (why this is safe in RS41ng):

- **TIM2** is RS41ng's data timer, but it only runs *during* a transmission
  (`data_timer_init()`/`data_timer_uninit()` around TX), and the boom is read
  in `telemetry_collect()`, which always runs *before* TX starts. Both users
  fully reconfigure the timer from scratch, so they cannot collide.
- **Interrupts off** for ~35 ms at a time is acceptable outside TX: GPS
  reception uses circular DMA (no bytes are lost while the CPU ignores
  interrupts), and the transmit scheduler is GPS-time-synced, so the few
  milliseconds of lost SysTick/scheduler ticks per telemetry collection do
  not accumulate into schedule drift.
- On the classic board the driver remaps **SWJ to SWD-only** so PB3/PB4 can
  be driven (see pin map above).
- All boom pins idle low, so a sonde without a boom attached (or with the
  feature disabled) leaves the analog section unpowered. When a channel does
  not oscillate, the capture times out, the affected readings are zeroed and
  the sensor is reported as failed for that cycle, mirroring the
  BMP280-handler error behavior.

Configuration (see `config.h`):

```
#define SENSOR_BOOM_ENABLE true       // read the original Vaisala sensors
#define SENSOR_BOOM_CAL_T        1.082771f   // calT      } from
#define SENSOR_BOOM_CAL_POLY_T0 -0.133869f   // polyT[0]  } extract_boom_calibration.ts
#define SENSOR_BOOM_CAL_POLY_T1  0.007139f   // polyT[1]  } (defaults: fleet averages)
#define SENSOR_BOOM_CAL_TU       1.317314f   // calTU
#define SENSOR_BOOM_CAL_POLY_TRH0 -0.210610f // polyTrh[0]
#define SENSOR_BOOM_CAL_POLY_TRH1  0.009892f // polyTrh[1]
#define SENSOR_BOOM_CAL_U0      43.257160f   // calibU[0]
#define SENSOR_BOOM_CAL_U1       5.067065f   // calibU[1]
```

### What RS41ng does not implement (and why)

The boom hardware supports additional machinery that RS41ng intentionally
leaves out, at least for now:

- **A generic (non-factory) calibration mode** — recovering temperature and
  humidity from the ideal PT1000 curve plus empirical corrections, without the
  per-sonde factory coefficients. This is useful when the factory coefficients
  are not available, but with the subframe extraction path the factory math is
  both simpler and more accurate, so RS41ng only implements the factory mode.
- **Humidity sensor heating, reconditioning and zero-humidity calibration**
  (`HEAT_HUM1/2` PWM, heat-to-140 °C cycles). These support de-icing and the
  generic humidity mode; the factory calibration does not require them.
  Without de-icing, expect the humidity reading to lag or saturate after
  passing through supercooled clouds — same caveat as any unheated sensor.
- **The reference heating resistors** (`HEAT_REF`, PC6): Vaisala uses them to
  keep the PCB reference section warm; not required for the measurement to
  work.
- **The pressure-dependent humidity correction.** Pressure itself is now
  supported on RS41-SGP sondes via the RPM411 module (`SENSOR_RPM411_ENABLE`,
  see [pressure-sensor.md](pressure-sensor.md)), but the full Vaisala humidity
  correction chain that uses it has not been ported; the empirical
  low-temperature correction above is used instead.
