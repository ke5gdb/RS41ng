#!/usr/bin/env python3
"""RS41 sensor boom calibration extractor (Python variant).

Reads the factory calibration ("subframe") data of an RS41 and writes the
per-sonde sensor boom coefficients into a supplemental configuration header,
src/config_boom_cal.h, which config.h picks up automatically (its values
override the fleet-average defaults and enable the sensor boom). See
docs/sensor-boom.md for background.

Usage:
    python3 scripts/extract_boom_calibration.py <path/to/*_subframe.bin>
    python3 scripts/extract_boom_calibration.py <serial>

A file argument must be a radiosonde_auto_rx subframe dump (800 or 816
bytes). A serial argument (e.g. X4643493) downloads the sonde's telemetry
from SondeHub, which works for any sonde that was fully received while
running the original Vaisala firmware.

Options:
    -o / --output PATH   write the header somewhere else
    --stdout             print the header instead of writing a file

This is the counterpart of extract_boom_calibration.ts for people who do not
use the config.yaml workflow; config.yaml builds should prefer
sensors.boom_calibration_file instead (the generated config does not read
config_boom_cal.h). Byte offsets follow einergehtnochrein's ra-firmware via
radiosonde_auto_rx/auto_rx/utils/rs41cal.py.

Exit codes: 0 = success, 1 = usage/parse/download error
"""

import argparse
import base64
import json
import struct
import sys
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUTPUT = REPO_ROOT / "src" / "config_boom_cal.h"

# Values compiled into RS41ng (src/boom_handler.c). Every sonde surveyed so
# far carries exactly these; a mismatch means boom_handler.c needs updating
# for this sonde.
EXPECTED_REF_RESISTOR = [750.0, 1100.0]
EXPECTED_REF_CAP = [0.0, 47.0]
EXPECTED_TAYLOR_T = [-243.910797, 0.187654004, 8.19999968e-6]
EXPECTED_MATRIX_U = [
    -0.0025859999, -2.24367189, 9.92294216, -3.61912608, 54.355381, -93.3011703,
    51.7056198, 38.8708611, 209.437378, -378.437012, 9.17325592, 19.5300655,
    150.257492, -150.906555, -280.314606, 182.293106, 3247.39429, 4083.6521,
    -233.568451, 345.374542, 200.216995, -388.245941, -3617.65796, 0,
    225.841125, -233.050598, 0, 0, 0, 0,
    -93.0635452, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0,
]


def die(message):
    print(f"ERROR: {message}", file=sys.stderr)
    sys.exit(1)


def parse_subframe(raw):
    """Extract the sensor boom calibration fields from a subframe dump."""
    if len(raw) not in (800, 816):
        die(f"subframe data must be 800 or 816 bytes, got {len(raw)}")

    def floats(offset, count=1):
        values = struct.unpack_from("<%df" % count, raw, offset)
        return values[0] if count == 1 else list(values)

    def string(offset, length):
        return raw[offset:offset + length].split(b"\x00")[0].decode(errors="replace")

    return {
        "serial": string(0x00D, 8),
        "variant": string(0x218, 10),
        "refResistor": [floats(0x03D), floats(0x041)],
        "refCap": [floats(0x045), floats(0x049)],
        "taylorT": floats(0x04D, 3),
        "calT": floats(0x059),
        "polyT": floats(0x05D, 6),
        "calibU": floats(0x075, 2),
        "matrixU": floats(0x07D, 42),
        "taylorTU": floats(0x125, 3),
        "calTU": floats(0x131),
        "polyTrh": floats(0x135, 6),
    }


def verify_universal_constants(cal):
    """Warn if the supposedly-universal coefficients differ from the firmware."""

    def nearly_equal(a, b):
        return abs(a - b) <= max(abs(a), abs(b)) * 1e-5

    def check(name, actual, expected):
        if not all(nearly_equal(a, e) for a, e in zip(actual, expected)):
            print(
                f"WARNING: {name} differs from the constants compiled into RS41ng:\n"
                f"    subframe: {actual}\n"
                f"    firmware: {expected}",
                file=sys.stderr,
            )

    check("refResistor", cal["refResistor"], EXPECTED_REF_RESISTOR)
    check("refCap", cal["refCap"], EXPECTED_REF_CAP)
    check("taylorT", cal["taylorT"], EXPECTED_TAYLOR_T)
    check("taylorTU", cal["taylorTU"], EXPECTED_TAYLOR_T)
    check("polyT[2..5]", cal["polyT"][2:], [0, 0, 0, 0])
    check("polyTrh[2..5]", cal["polyTrh"][2:], [0, 0, 0, 0])
    check("matrixU", cal["matrixU"], EXPECTED_MATRIX_U)


def download_subframe(serial):
    """Fetch telemetry from SondeHub and return the first embedded subframe."""
    url = f"https://api.v2.sondehub.org/sonde/{serial}"
    print(f"Downloading telemetry for {serial} from SondeHub...", file=sys.stderr)
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            frames = json.load(response)
    except Exception as e:
        die(f"SondeHub request failed: {e}")
    if not isinstance(frames, list):
        die("unexpected SondeHub response (not a frame list)")
    for frame in frames:
        subframe = frame.get("rs41_subframe")
        if isinstance(subframe, str):
            return base64.b64decode(subframe)
    die(
        f"no subframe found in {len(frames)} SondeHub frames for {serial} "
        "(the sonde must have been received telemetry-complete)"
    )


def generate_header(cal):
    """Render the supplemental configuration header."""
    g = lambda v: "%.9g" % v  # enough digits to round-trip an IEEE-754 single
    defines = [
        ("SENSOR_BOOM_ENABLE", "true", "read the original Vaisala sensors"),
        ("SENSOR_BOOM_CAL_T", g(cal["calT"]) + "f", "calT: main temperature resistance gain"),
        ("SENSOR_BOOM_CAL_POLY_T0", g(cal["polyT"][0]) + "f", "polyT[0]: main temperature offset"),
        ("SENSOR_BOOM_CAL_POLY_T1", g(cal["polyT"][1]) + "f", "polyT[1]: main temperature scale trim"),
        ("SENSOR_BOOM_CAL_TU", g(cal["calTU"]) + "f", "calTU: humidity-sensor temperature resistance gain"),
        ("SENSOR_BOOM_CAL_POLY_TRH0", g(cal["polyTrh"][0]) + "f", "polyTrh[0]: humidity-sensor temperature offset"),
        ("SENSOR_BOOM_CAL_POLY_TRH1", g(cal["polyTrh"][1]) + "f", "polyTrh[1]: humidity-sensor temperature scale trim"),
        ("SENSOR_BOOM_CAL_U0", g(cal["calibU"][0]) + "f", "calibU[0]: humidity capacitance normalization"),
        ("SENSOR_BOOM_CAL_U1", g(cal["calibU"][1]) + "f", "calibU[1]: humidity capacitance scale"),
    ]
    width = max(len(f"#define {name} {value}") for name, value, _ in defines)
    lines = [
        f"// Sensor boom factory calibration for {cal['serial']} ({cal['variant']})",
        "// Generated by scripts/extract_boom_calibration.py - DO NOT EDIT MANUALLY",
        "// Included by config.h; these values override the fleet-average defaults",
        "// and enable the sensor boom. Delete this file to revert to the defaults.",
        "// See docs/sensor-boom.md for details.",
        "",
        "#ifndef __CONFIG_BOOM_CAL_H",
        "#define __CONFIG_BOOM_CAL_H",
        "",
    ]
    for name, value, comment in defines:
        define = f"#define {name} {value}"
        lines.append(f"{define:{width}} // {comment}")
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(
        description="Extract RS41 sensor boom calibration into src/config_boom_cal.h"
    )
    parser.add_argument(
        "source",
        help="a radiosonde_auto_rx subframe dump (*.bin) or an RS41 serial number",
    )
    parser.add_argument(
        "-o", "--output",
        type=Path,
        default=DEFAULT_OUTPUT,
        help=f"output header path (default: {DEFAULT_OUTPUT})",
    )
    parser.add_argument(
        "--stdout",
        action="store_true",
        help="print the header to stdout instead of writing a file",
    )
    args = parser.parse_args()

    if args.source.endswith(".bin"):
        try:
            raw = Path(args.source).read_bytes()
        except OSError as e:
            die(f"failed to read {args.source}: {e}")
    else:
        raw = download_subframe(args.source)

    cal = parse_subframe(raw)
    verify_universal_constants(cal)

    header = generate_header(cal)

    if args.stdout:
        print(header, end="")
        return

    args.output.write_text(header)
    print(f"Sensor boom calibration for {cal['serial']} ({cal['variant']}) written to {args.output}")


if __name__ == "__main__":
    main()
