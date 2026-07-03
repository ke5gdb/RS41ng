#include "config.h"

#if defined(RS41) && SENSOR_RPM411_ENABLE

#include <math.h>

#include "rpm411_handler.h"
#include "drivers/rpm411/rpm411.h"
#include "log.h"

static bool rpm411_initialization_required = true;

bool rpm411_handler_init()
{
    char serial[RPM411_SERIAL_LENGTH + 1];

    bool success = rpm411_init(serial);
    if (success) {
        log_info("RPM411 pressure module found, serial: %s\n", serial);
    } else {
        log_error("RPM411 pressure module not responding\n");
    }

    rpm411_initialization_required = !success;
    return success;
}

bool rpm411_read_telemetry(telemetry_data *data)
{
    float pressure_hpa = 0.0f;
    float module_temperature = 0.0f;
    bool success = false;

    if (!rpm411_initialization_required) {
        success = rpm411_read(&pressure_hpa, &module_temperature);
    }

    if (!success) {
        // One re-init attempt, mirroring the BMP280 handler (and the original
        // firmware, which repeats the configuration readout after errors)
        log_info("RPM411 re-init\n");
        if (rpm411_handler_init()) {
            success = rpm411_read(&pressure_hpa, &module_temperature);
        }
    }

    if (!success) {
        rpm411_initialization_required = true;
        log_error("RPM411 read failed\n");
        if (LEDS_ENABLE) {
            set_red_led(true);
        }
#if !(SENSOR_BMP280_ENABLE || SENSOR_BME68X_ENABLE || SENSOR_BME690_ENABLE)
        // No other sensor provides pressure; zero it so stale readings are
        // not transmitted
        data->pressure_mbar_100 = 0;
#endif
        if (data->ext_sensor_type == SENSOR_RPM411) {
            data->ext_sensor_type = NO_EXT_SENSOR;
        }
        return false;
    }

    log_info("RPM411: P %lu.%02u hPa, module T %ld cC\n",
            (unsigned long) pressure_hpa,
            (unsigned int) (lroundf(pressure_hpa * 100.0f) % 100),
            lroundf(module_temperature * 100.0f));

    // hPa == mbar; overrides any I2C sensor pressure read earlier in the cycle
    data->pressure_mbar_100 = (uint32_t) lroundf(pressure_hpa * 100.0f);

    // Claim the external sensor slot only when nothing else (boom, I2C
    // sensors) did: the RPM411 provides pressure only
    if (data->ext_sensor_type == NO_EXT_SENSOR) {
        data->ext_sensor_type = SENSOR_RPM411;
    }

    return true;
}

#endif
