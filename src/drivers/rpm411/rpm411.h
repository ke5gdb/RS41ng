#ifndef __RPM411_H
#define __RPM411_H

#include <stdbool.h>

/**
 * Driver for the original Vaisala RPM411 barometric pressure module (RS41-SGP).
 *
 * The RPM411 is a mezzanine board on the expansion connector, present only on
 * sondes ordered with the pressure option. It carries its own STM32F100 that
 * runs the BAROCAP ring-oscillator measurement and applies the factory
 * calibration internally, so this driver receives finished physical values:
 * pressure in hPa and the module's internal temperature in degrees C, both as
 * IEEE-754 floats.
 *
 * The module shares the radio SPI bus with a dedicated chip select, and needs
 * a measurement clock supplied by the main MCU on MCO1/PA8. Commands are
 * opcode + CRC-16/CCITT-FALSE + pad; responses are CRC-framed. The protocol was
 * reverse-engineered from the module firmware and the live bus.
 * See docs/pressure-sensor.md for details.
 */

#define RPM411_SERIAL_LENGTH 8

/**
 * Enable the measurement clock, then probe the module and read its
 * configuration/calibration pages. On success the module serial number (8 ASCII
 * characters) is stored in the given buffer, NUL-terminated.
 *
 * Returns false when no module responds (sonde without the pressure option).
 */
bool rpm411_init(char serial[RPM411_SERIAL_LENGTH + 1]);

/**
 * Trigger a measurement and read it out. Blocks for the module's ~250 ms
 * conversion time, so call only from telemetry collection (the SPI bus must
 * not be in use by the radio).
 *
 * Returns false if the module does not respond or returns values outside
 * physical limits; the outputs are left untouched in that case.
 */
bool rpm411_read(float *pressure_hpa, float *internal_temperature_celsius);

#endif
