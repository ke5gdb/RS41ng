/**
 * RS41 factory calibration ("subframe") parsing for the sensor boom.
 *
 * Environment-agnostic: operates on a Uint8Array so it can be used both by
 * the CLI scripts (reading .bin files / SondeHub downloads) and by the web
 * configurator. Byte offsets follow einergehtnochrein's ra-firmware via
 * radiosonde_auto_rx/auto_rx/utils/rs41cal.py. See docs/sensor-boom.md.
 */

export interface BoomCalibration {
  serial: string;
  variant: string;
  refResistor: number[]; // [low, high] ohm
  refCap: number[]; // [low, high] pF
  taylorT: number[]; // [3]
  calT: number;
  polyT: number[]; // [6]
  calibU: number[]; // [2]
  matrixU: number[]; // [42]
  taylorTU: number[]; // [3]
  calTU: number;
  polyTrh: number[]; // [6]
}

/** The per-sonde values as config field key/value pairs (sensors section) */
export function boomConfigValues(cal: BoomCalibration): Record<string, number> {
  return {
    boom_cal_t: cal.calT,
    boom_cal_poly_t0: cal.polyT[0],
    boom_cal_poly_t1: cal.polyT[1],
    boom_cal_tu: cal.calTU,
    boom_cal_poly_trh0: cal.polyTrh[0],
    boom_cal_poly_trh1: cal.polyTrh[1],
    boom_cal_u0: cal.calibU[0],
    boom_cal_u1: cal.calibU[1],
  };
}

function f32(data: DataView, offset: number): number {
  return data.getFloat32(offset, true);
}

function f32Array(data: DataView, offset: number, count: number): number[] {
  return Array.from({ length: count }, (_, i) => f32(data, offset + 4 * i));
}

function str(data: DataView, offset: number, length: number): string {
  const bytes = new Uint8Array(data.buffer, data.byteOffset + offset, length);
  let end = bytes.indexOf(0);
  if (end < 0) end = length;
  return new TextDecoder().decode(bytes.subarray(0, end));
}

/**
 * Parse a subframe dump (800 bytes of calibration data, or 816 bytes as
 * written by radiosonde_auto_rx with the runtime-variable block appended).
 * Throws on invalid input.
 */
export function parseSubframe(raw: Uint8Array): BoomCalibration {
  if (raw.length !== 800 && raw.length !== 816) {
    throw new Error(`subframe data must be 800 or 816 bytes, got ${raw.length}`);
  }
  const d = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  return {
    serial: str(d, 0x00d, 8),
    variant: str(d, 0x218, 10),
    refResistor: [f32(d, 0x03d), f32(d, 0x041)],
    refCap: [f32(d, 0x045), f32(d, 0x049)],
    taylorT: f32Array(d, 0x04d, 3),
    calT: f32(d, 0x059),
    polyT: f32Array(d, 0x05d, 6),
    calibU: f32Array(d, 0x075, 2),
    matrixU: f32Array(d, 0x07d, 42),
    taylorTU: f32Array(d, 0x125, 3),
    calTU: f32(d, 0x131),
    polyTrh: f32Array(d, 0x135, 6),
  };
}

// Values compiled into RS41ng (src/boom_handler.c). Every sonde surveyed so far
// carries exactly these; a mismatch means boom_handler.c needs updating for
// this sonde.
const EXPECTED_REF_RESISTOR = [750.0, 1100.0];
const EXPECTED_REF_CAP = [0.0, 47.0];
const EXPECTED_TAYLOR_T = [-243.910797, 0.187654004, 8.19999968e-6];
// prettier-ignore
const EXPECTED_MATRIX_U = [
  -0.0025859999, -2.24367189, 9.92294216, -3.61912608, 54.355381, -93.3011703,
  51.7056198, 38.8708611, 209.437378, -378.437012, 9.17325592, 19.5300655,
  150.257492, -150.906555, -280.314606, 182.293106, 3247.39429, 4083.6521,
  -233.568451, 345.374542, 200.216995, -388.245941, -3617.65796, 0,
  225.841125, -233.050598, 0, 0, 0, 0,
  -93.0635452, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0,
];

function nearlyEqual(a: number, b: number): boolean {
  return Math.abs(a - b) <= Math.max(Math.abs(a), Math.abs(b)) * 1e-5;
}

/**
 * Check the supposedly-universal coefficients against the constants baked
 * into the firmware. Returns human-readable warnings (empty = all match).
 */
export function verifyUniversalConstants(cal: BoomCalibration): string[] {
  const warnings: string[] = [];
  const check = (name: string, actual: number[], expected: number[]) => {
    if (!actual.every((v, i) => nearlyEqual(v, expected[i]))) {
      warnings.push(
        `${name} differs from the constants compiled into RS41ng:\n` +
          `    subframe: [${actual.join(", ")}]\n` +
          `    firmware: [${expected.join(", ")}]`
      );
    }
  };
  check("refResistor", cal.refResistor, EXPECTED_REF_RESISTOR);
  check("refCap", cal.refCap, EXPECTED_REF_CAP);
  check("taylorT", cal.taylorT, EXPECTED_TAYLOR_T);
  check("taylorTU", cal.taylorTU, EXPECTED_TAYLOR_T);
  check("polyT[2..5]", cal.polyT.slice(2), [0, 0, 0, 0]);
  check("polyTrh[2..5]", cal.polyTrh.slice(2), [0, 0, 0, 0]);
  check("matrixU", cal.matrixU, EXPECTED_MATRIX_U);
  return warnings;
}

/** Format a number with enough digits to round-trip an IEEE-754 single */
export function formatCalValue(v: number): string {
  const s = Number(v.toPrecision(9)).toString();
  return s.includes(".") || s.includes("e") ? s : `${s}.0`;
}
