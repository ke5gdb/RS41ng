#!/usr/bin/env bun
/**
 * RS41 sensor boom calibration extractor
 *
 * Reads the factory calibration ("subframe") data of an RS41 and emits the
 * per-sonde SENSOR_BOOM_CAL_* configuration for RS41ng. See
 * docs/sensor-boom.md for background.
 *
 * Usage:
 *   bun run scripts/extract_boom_calibration.ts <path/to/*_subframe.bin>
 *   bun run scripts/extract_boom_calibration.ts <serial>
 *
 * A file argument must be a radiosonde_auto_rx subframe dump (800 or 816
 * bytes). A serial argument (e.g. X4643493) downloads the sonde's telemetry
 * from SondeHub and extracts the subframe from it, which works for any sonde
 * that was fully received while running the original Vaisala firmware.
 *
 * NOTE: For config.yaml-based builds you do not need to paste these values:
 * point sensors.boom_calibration_file at the subframe .bin instead and the
 * config generator extracts the coefficients at build time.
 *
 * Exit codes: 0 = success, 1 = usage/parse/download error
 */

import { readFileSync } from "fs";

// Shared with the config generator (and the web configurator sources)
import {
  parseSubframe,
  verifyUniversalConstants,
  boomConfigValues,
  formatCalValue as g,
  type BoomCalibration,
} from "../web/src/config/boom-subframe";

function die(message: string): never {
  console.error(`ERROR: ${message}`);
  process.exit(1);
}

// ─── SondeHub download ─────────────────────────────────────────────────────────

async function downloadSubframe(serial: string): Promise<Uint8Array> {
  const url = `https://api.v2.sondehub.org/sonde/${encodeURIComponent(serial)}`;
  console.error(`Downloading telemetry for ${serial} from SondeHub...`);
  const response = await fetch(url);
  if (!response.ok) {
    die(`SondeHub request failed: ${response.status} ${response.statusText}`);
  }
  const frames = (await response.json()) as Array<Record<string, unknown>>;
  if (!Array.isArray(frames)) {
    die("unexpected SondeHub response (not a frame list)");
  }
  for (const frame of frames) {
    if (typeof frame.rs41_subframe === "string") {
      return Uint8Array.from(Buffer.from(frame.rs41_subframe, "base64"));
    }
  }
  die(
    `no subframe found in ${frames.length} SondeHub frames for ${serial} ` +
      "(the sonde must have been received telemetry-complete)"
  );
}

// ─── Output ───────────────────────────────────────────────────────────────────

const DEFINE_BY_FIELD: Record<string, string> = {
  boom_cal_t: "SENSOR_BOOM_CAL_T",
  boom_cal_poly_t0: "SENSOR_BOOM_CAL_POLY_T0",
  boom_cal_poly_t1: "SENSOR_BOOM_CAL_POLY_T1",
  boom_cal_tu: "SENSOR_BOOM_CAL_TU",
  boom_cal_poly_trh0: "SENSOR_BOOM_CAL_POLY_TRH0",
  boom_cal_poly_trh1: "SENSOR_BOOM_CAL_POLY_TRH1",
  boom_cal_u0: "SENSOR_BOOM_CAL_U0",
  boom_cal_u1: "SENSOR_BOOM_CAL_U1",
};

function printConfig(cal: BoomCalibration) {
  const values = boomConfigValues(cal);

  console.log(`// Sensor boom factory calibration for ${cal.serial} (${cal.variant})`);
  console.log(`#define SENSOR_BOOM_ENABLE true`);
  for (const [key, define] of Object.entries(DEFINE_BY_FIELD)) {
    console.log(`#define ${define} ${g(values[key])}f`);
  }
  console.log();
  console.log(`# Equivalent config.yaml block (sensor boom calibration for ${cal.serial}):`);
  console.log(`sensors:`);
  console.log(`  boom_enable: true`);
  for (const key of Object.keys(DEFINE_BY_FIELD)) {
    console.log(`  ${key}: ${g(values[key])}`);
  }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

async function main() {
  const arg = process.argv[2];
  if (!arg) {
    die(
      "usage: bun run scripts/extract_boom_calibration.ts <subframe.bin | serial>"
    );
  }

  const raw = arg.endsWith(".bin")
    ? new Uint8Array(readFileSync(arg))
    : await downloadSubframe(arg);

  let cal: BoomCalibration;
  try {
    cal = parseSubframe(raw);
  } catch (e: any) {
    die(e.message);
  }

  for (const warning of verifyUniversalConstants(cal)) {
    console.error(`WARNING: ${warning}`);
  }

  printConfig(cal);
}

main();
