# The original Vaisala RPM411 pressure module (RS41-SGP)

RS41ng can read barometric pressure from the sonde's original RPM411 pressure
module with `SENSOR_RPM411_ENABLE` (or `rpm411_enable: true` under `sensors:`
in `config.yaml`).

The pressure module was a **factory option**: sondes sold as RS41-SGP have it,
the far more common RS41-SG (and RS41-NG) do not. It is a separate mezzanine
board plugged into the main board's internal expansion connector, so its
presence is independent of the sensor boom — enable it only on sondes that
actually have the extra board. If it is enabled on a sonde without the module,
RS41ng logs an error at startup and simply reports no pressure; everything
else keeps working.

## How the module works

Unlike the boom sensors (see [sensor-boom.md](sensor-boom.md)), which are raw
oscillator channels measured and converted by the main MCU, the RPM411 is a
self-contained instrument:

- It carries its **own STM32F100C8**, running its own firmware.
- A BAROCAP capacitive pressure sensor and two on-board reference capacitors
  are switched one at a time into a relaxation oscillator on the module. The
  oscillator's frequency depends on the connected capacitance, so the unknown
  sensor capacitance is recovered by interpolating between the two known
  references measured on the same oscillator — the same ratiometric idea the
  boom uses for its resistive and capacitive channels.
- The module's MCU counts those oscillator frequencies **and applies the
  factory calibration internally**. The host receives finished physical
  values: pressure in hPa and the module's internal (compensation) temperature
  in °C, both as little-endian IEEE-754 floats.

The practical consequence: **no calibration extraction is needed**. There are
no per-sonde coefficients to configure; the calibration lives on the module
itself. (The original ground-station telemetry also carried the raw counts and
a `matP` calibration polynomial so the pressure could be recomputed on the
ground, but that path is unnecessary here — the module already produces the
calibrated result.)

Hardware documentation of the module, including full schematics, is available
in [TjarkG/RPM411](https://github.com/TjarkG/RPM411); the expansion connector
pinout is covered by [bazjo's RS41 hardware RE](https://github.com/bazjo/RS41_Hardware),
and the module firmware has been extracted and studied
([analogic.cz](https://www.analogic.cz/rs41-rpm411/)). The protocol and timing
described below were reverse-engineered from a disassembly of that firmware
together with logic-analyzer observation of the live bus.

## The measurement clock (essential)

The RPM411 has **no local timebase for its measurement engine**. Its
frequency-counting logic is clocked from an external reference that the main
board must supply on the connector — labelled `RPM_CLOCK` in the schematic and
wired to the STM32's clock-output pin **MCO1 / PA8**.

Without this clock the module still powers up, answers commands and returns its
calibration and serial number perfectly — but every oscillator-channel capture
reads **zero counts**, and the on-board conversion of "all zeros" produces a
fixed nonsense pressure (around −245 hPa). This is the single most important
thing to get right; it is easy to mistake the healthy-but-silent module for a
protocol problem.

RS41ng drives PA8 from **HSE** — the RS41's 24 MHz external oscillator, which
already clocks the whole system (`SYSCLK`), so it is always running. HSE is
used in preference to the internal HSI RC oscillator for two reasons:

1. **Lower jitter.** As explained under *Measurement noise* below, the pressure
   is a ratio of nearby oscillator counts, which amplifies any timebase jitter.
   A crystal/TCXO-grade HSE is far quieter than the HSI RC oscillator.
2. **It is almost certainly the native configuration.** The 24 MHz system clock
   is the natural thing for the original main board to have fed the module on
   this pin. Because the pressure is ratiometric, the exact clock frequency
   does not affect the calibrated result — only its stability matters — so any
   clean frequency is safe.

The clock is enabled once at driver init and left running for the life of the
firmware.

## Electrical interface

The module sits on the **shared SPI bus** (SPI2, PB13/PB14/PB15) together with
the Si4032 radio, with its own chip select on **PB2**. RS41ng drives PB2 high
(deselected) from GPIO init onwards — also on sondes without the module, where
the pin goes to an unloaded connector — so the module never sees the radio's
SPI traffic.

Power comes from the same 3.0 V and 3.8 V rails as the rest of the sonde; no
enable or sequencing is required beyond what RS41ng already does. The module's
own analog front-end is fed from the 3.8 V boost rail through a local
regulator.

## Protocol

Commands are short and self-describing: an **opcode** and its arguments,
followed by a **16-bit CRC** and a trailing pad byte. The CRC is
**CRC-16/CCITT-FALSE** (polynomial `0x1021`, initial value `0xFFFF`) over the
opcode bytes, transmitted low byte first. RS41ng computes the CRC itself, so it
can build any command rather than replaying fixed byte sequences.

Framing details, all confirmed against the module firmware:

- Chip select is toggled around **every single byte**, with roughly 100 µs of
  settling on each side. Exchanges are a fixed 33 bytes long: the command
  bytes, a ~450 µs processing pause, then zero bytes clocked out to receive the
  response.
- The module **echoes each received byte back one byte late**. RS41ng uses this
  to confirm a command was actually heard.
- A response is itself framed as `<length> <payload…> <crc16>` (CRC low byte
  first, computed over the length and payload). RS41ng validates this CRC and
  rejects any frame that does not check out.
- After serving a response the module is **busy for ~10 ms** and ignores the
  start of the next command. RS41ng therefore retries each command a few times
  with a short pause, using the byte-echo / CRC checks to know when a command
  landed.

Three opcodes are used:

- **Configuration read** (`0x03`): reads a page by 16-bit address. At startup
  RS41ng walks the calibration/configuration pages this way; the page at
  address `0x0028` carries the 8-character ASCII **module serial number**,
  which is logged.
- **Measure** (`0x01`): triggers a conversion. RS41ng waits **250 ms** for it
  to complete.
- **Readout** (`0x02`): returns the 22-byte measurement payload.

Within the measurement payload:

- three 32-bit raw oscillator counts (BAROCAP sensor, reference 1, reference 2)
  at offsets 0, 4 and 8,
- the module's internal temperature as a float at offset 14,
- the barometric pressure (hPa) as a float at offset 18.

Module detection is by response content and plausibility: with no module fitted
the MISO line floats and responses fail their CRC or read as constant 0xFF/0x00,
and readings are range-checked (0–1200 hPa, ±100 °C) — which also rejects NaN
from a floating bus.

## Measurement noise

The module already averages several samples internally, but the residual noise
is worth understanding. The pressure is derived by interpolating the sensor
capacitance **between the two reference capacitors** — effectively a difference
of nearby counts divided by a difference of nearby counts. That ratio strongly
amplifies small count errors: in practice a ~0.03 % wobble in the raw counts
becomes roughly ±6 hPa of scatter in the reported pressure (about an 18×
amplification). Because the reference capacitors are physically fixed, any
variation in their counts is pure measurement noise rather than signal.

That noise has two components:

- a **common-mode** part from timebase jitter, addressed by clocking the module
  from the low-jitter HSE rather than HSI (see *The measurement clock* above);
- an **independent** part from analog and supply noise, which is worst when the
  battery is low or the supply rail is sagging — a marginal battery also
  destabilises the boom, so a healthy supply helps both sensors.

If further smoothing is wanted, pressure is a slowly varying quantity and
tolerates a light temporal low-pass filter well; the only caveat for flight is
to keep the filter responsive enough not to lag genuine pressure change during
ascent and descent.

## Integration notes

- The measurement (including the 250 ms conversion wait) runs inside
  `telemetry_collect()` only, which never overlaps a transmission — the SPI
  bus is free and the radio's chip select is deasserted there.
- The RPM411 fills only the pressure telemetry field. Combined with the
  sensor boom (the natural RS41-SGP configuration) this reproduces the full
  original PTU measurement set. Combined with an I²C sensor (BMP280/BME280
  etc.), the RPM411 reading, taken last, **overrides** the I²C pressure.
- If the module stops responding mid-flight, the handler re-runs the
  initialization sequence once per telemetry cycle and otherwise reports no
  pressure rather than a stale value.
- The module's internal temperature is logged but not transmitted: it is the
  temperature inside the sonde housing, not the outside air.

## What is not implemented

- **Reconfiguring the module.** RS41ng only reads calibration, triggers
  conversions and reads results. The module also exposes operational commands
  (channel selection, sleep/mode control) over a separate serial interface that
  is not wired to the RS41 main board on the standard connector; these are not
  used.
- **The pressure-dependent humidity correction** in the boom's humidity
  conversion; the boom code keeps its empirical low-temperature correction.
  See [sensor-boom.md](sensor-boom.md).
