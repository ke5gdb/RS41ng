#include "config.h"

#if defined(RS41) && SENSOR_BOOM_ENABLE

#include <math.h>

#include "boom_handler.h"
#include "drivers/boom/boom.h"
#include "log.h"

/**
 * Conversion of sensor boom oscillator frequencies to physical values using
 * the Vaisala factory calibration, as reverse engineered by rs1729/RS. See
 * docs/sensor-boom.md for the math and the origin of the coefficients.
 *
 * The constants below are identical on every RS41 surveyed (149 sondes);
 * only the eight SENSOR_BOOM_CAL_* values in config.h are per-sonde.
 */

static const float boom_ref_resistor_low_ohm = 750.0f;
static const float boom_ref_resistor_high_ohm = 1100.0f;
static const float boom_ref_cap_low_pf = 0.0f;
static const float boom_ref_cap_high_pf = 47.0f;

// Resistance-to-temperature polynomial for both platinum sensors
// (subframe taylorT == taylorTU on all sondes)
static const float boom_taylor_t[3] = {-243.910797f, 0.187654004f, 8.19999968e-06f};

// Vaisala humidity calibration surface in normalized capacitance (rows) and
// normalized humidity-sensor temperature (columns), subframe matrixU
static const float boom_matrix_u[42] = {
        -0.0025859999f, -2.24367189f, 9.92294216f, -3.61912608f, 54.355381f, -93.3011703f,
        51.7056198f, 38.8708611f, 209.437378f, -378.437012f, 9.17325592f, 19.5300655f,
        150.257492f, -150.906555f, -280.314606f, 182.293106f, 3247.39429f, 4083.6521f,
        -233.568451f, 345.374542f, 200.216995f, -388.245941f, -3617.65796f, 0.0f,
        225.841125f, -233.050598f, 0.0f, 0.0f, 0.0f, 0.0f,
        -93.0635452f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
};

/**
 * Temperature of a platinum sensor channel.
 *
 * The calibration math interpolates values proportional to the oscillator
 * period, so frequencies are fed in as reciprocals; any constant scale factor
 * cancels between the sensor and reference channels.
 *
 * cal_gain/poly_offset/poly_scale are the per-sonde coefficients: calT/polyT
 * for the main sensor, calTU/polyTrh for the humidity-sensor PT1000.
 */
static float boom_temperature_celsius(float freq, float freq_ref_low, float freq_ref_high,
        float cal_gain, float poly_offset, float poly_scale)
{
    float m = 1.0f / freq;
    float m1 = 1.0f / freq_ref_low;
    float m2 = 1.0f / freq_ref_high;

    float gain = (m2 - m1) / (boom_ref_resistor_high_ohm - boom_ref_resistor_low_ohm);
    float offset = (m1 * boom_ref_resistor_high_ohm - m2 * boom_ref_resistor_low_ohm) / (m2 - m1);
    float resistance = (m / gain - offset) * cal_gain;

    return (boom_taylor_t[0]
            + boom_taylor_t[1] * resistance
            + boom_taylor_t[2] * resistance * resistance
            + poly_offset) * (1.0f + poly_scale);
}

// Water vapor saturation pressure in Pa (Hyland and Wexler)
static float boom_vapor_saturation_pressure(float temperature_celsius)
{
    float t = temperature_celsius + 273.15f;
    return expf(-5800.2206f / t
                + 1.3914993f
                + 6.5459673f * logf(t)
                - 4.8640239e-2f * t
                + 4.1764768e-5f * t * t
                - 1.4452093e-8f * t * t * t);
}

/**
 * Relative humidity referenced to air temperature.
 *
 * temp_air is the main sensor temperature, temp_sensor the temperature of the
 * (slightly warmer) humidity sensor itself: RH is measured at the sensor and
 * rescaled to air temperature via the saturation vapor pressure ratio.
 */
static float boom_humidity_percentage(float freq, float freq_ref_low, float freq_ref_high,
        float temp_air, float temp_sensor)
{
    float m = 1.0f / freq;
    float m1 = 1.0f / freq_ref_low;
    float m2 = 1.0f / freq_ref_high;

    float cfh = (m - m1) / (m2 - m1);
    float capacitance = boom_ref_cap_low_pf + (boom_ref_cap_high_pf - boom_ref_cap_low_pf) * cfh;
    float cp = (capacitance / SENSOR_BOOM_CAL_U0 - 1.0f) * SENSOR_BOOM_CAL_U1;

    float trh = (temp_sensor - 20.0f) / 180.0f;
    float trh_powers[6];
    trh_powers[0] = 1.0f;
    for (int k = 1; k < 6; k++) {
        trh_powers[k] = trh_powers[k - 1] * trh;
    }

    float humidity = 0.0f;
    float cp_power = 1.0f;
    for (int j = 0; j < 7; j++) {
        for (int k = 0; k < 6; k++) {
            humidity += cp_power * trh_powers[k] * boom_matrix_u[6 * j + k];
        }
        cp_power *= cp;
    }

    // Empirical low-temperature correction (used in place of the
    // pressure-dependent correction, which needs a pressure measurement)
    if (temp_air < -40.0f) {
        humidity += (temp_air + 40.0f) / 12.0f;
    }

    humidity *= boom_vapor_saturation_pressure(temp_sensor) / boom_vapor_saturation_pressure(temp_air);

    if (humidity < 0.0f) {
        humidity = 0.0f;
    }
    if (humidity > 100.0f) {
        humidity = 100.0f;
    }
    return humidity;
}

bool boom_handler_init()
{
    // The reference channels exercise the oscillator and analog switches
    // without requiring the boom itself, so this verifies the on-board
    // measurement circuit. The 750 ohm reference must oscillate faster than
    // the 1100 ohm reference.
    float freq_ref_low = boom_measure_frequency(BOOM_CHANNEL_REF_750R);
    float freq_ref_high = boom_measure_frequency(BOOM_CHANNEL_REF_1100R);

    log_info("Boom references: %lu Hz (750R), %lu Hz (1100R)\n",
            (unsigned long) freq_ref_low, (unsigned long) freq_ref_high);

    return freq_ref_low > 0.0f && freq_ref_high > 0.0f && freq_ref_low > freq_ref_high;
}

bool boom_read_telemetry(telemetry_data *data)
{
    // Re-measure the references on every cycle so the ratiometric conversion
    // tracks oscillator drift
    float freq_ref_low = boom_measure_frequency(BOOM_CHANNEL_REF_750R);
    float freq_ref_high = boom_measure_frequency(BOOM_CHANNEL_REF_1100R);
    float freq_temp_main = boom_measure_frequency(BOOM_CHANNEL_TEMP_MAIN);
    float freq_temp_humidity = boom_measure_frequency(BOOM_CHANNEL_TEMP_HUMIDITY_SENSOR);
    float freq_humidity = boom_measure_frequency(BOOM_CHANNEL_HUMIDITY);
    float freq_cap_low = boom_measure_frequency(BOOM_CHANNEL_REF_CAP_LOW);
    float freq_cap_high = boom_measure_frequency(BOOM_CHANNEL_REF_CAP_HIGH);

    bool references_valid = freq_ref_low > 0.0f && freq_ref_high > 0.0f && freq_ref_low > freq_ref_high;
    bool temperature_valid = references_valid && freq_temp_main > 0.0f;
    bool humidity_valid = references_valid && freq_temp_humidity > 0.0f && freq_humidity > 0.0f
                          && freq_cap_low > 0.0f && freq_cap_high > 0.0f && freq_cap_low != freq_cap_high;

    float temperature_main = 0.0f;
    float humidity = 0.0f;

    if (temperature_valid) {
        temperature_main = boom_temperature_celsius(freq_temp_main, freq_ref_low, freq_ref_high,
                SENSOR_BOOM_CAL_T, SENSOR_BOOM_CAL_POLY_T0, SENSOR_BOOM_CAL_POLY_T1);
    }
    if (humidity_valid && temperature_valid) {
        float temperature_humidity_sensor = boom_temperature_celsius(freq_temp_humidity, freq_ref_low, freq_ref_high,
                SENSOR_BOOM_CAL_TU, SENSOR_BOOM_CAL_POLY_TRH0, SENSOR_BOOM_CAL_POLY_TRH1);
        humidity = boom_humidity_percentage(freq_humidity, freq_cap_low, freq_cap_high,
                temperature_main, temperature_humidity_sensor);

        log_info("Boom: T %ld cC (sensor %ld cC), RH %ld c%%\n",
                lroundf(temperature_main * 100.0f),
                lroundf(temperature_humidity_sensor * 100.0f),
                lroundf(humidity * 100.0f));
    }

    if (!temperature_valid || !humidity_valid) {
        log_error("Boom read failed (refs %lu/%lu, T %lu, TH %lu, U %lu, C %lu/%lu Hz)\n",
                (unsigned long) freq_ref_low, (unsigned long) freq_ref_high,
                (unsigned long) freq_temp_main, (unsigned long) freq_temp_humidity,
                (unsigned long) freq_humidity,
                (unsigned long) freq_cap_low, (unsigned long) freq_cap_high);
        if (LEDS_ENABLE) {
            set_red_led(true);
        }
#if !(SENSOR_BMP280_ENABLE || SENSOR_BME68X_ENABLE || SENSOR_BME690_ENABLE)
        // No other sensor provides these fields; zero them so stale readings
        // are not transmitted (mirrors the BMP280 handler behavior)
        data->temperature_celsius_100 = 0;
        data->humidity_percentage_100 = 0;
        data->ext_sensor_type = NO_EXT_SENSOR;
#endif
        return false;
    }

    data->temperature_celsius_100 = lroundf(temperature_main * 100.0f);
    data->humidity_percentage_100 = (uint32_t) lroundf(humidity * 100.0f);
    data->ext_sensor_type = SENSOR_BOOM;

    return true;
}

#endif
