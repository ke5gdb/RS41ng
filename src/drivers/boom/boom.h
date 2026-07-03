#ifndef __BOOM_H
#define __BOOM_H

/**
 * Driver for the original Vaisala RS41 sensor boom measurement circuit.
 *
 * The boom sensors (PT1000 temperature sensors and a capacitive humidity
 * sensor) and a set of on-board references are switched one at a time into a
 * relaxation oscillator on the main PCB. The oscillator period is close to a
 * linear function of the connected resistance/capacitance, so unknown sensor
 * values are recovered by interpolating between two known references measured
 * on the same oscillator.
 *
 * This driver provides channel selection and frequency measurement only;
 * conversion to physical units lives in boom_handler.c.
 * See docs/sensor-boom.md for the full interface description.
 */

// Channel numbering: resistive (temperature-rail) channels first, then capacitive
typedef enum _boom_channel {
    // Resistive channels (temperature rail)
    BOOM_CHANNEL_REF_750R = 1,           // 750 ohm reference resistor
    BOOM_CHANNEL_REF_1100R,              // 1100 ohm reference resistor
    BOOM_CHANNEL_TEMP_HUMIDITY_SENSOR,   // PT1000 on the humidity sensor
    BOOM_CHANNEL_TEMP_MAIN,              // PT1000 for main air temperature
    // Capacitive channels (humidity rail)
    BOOM_CHANNEL_HUMIDITY,               // capacitive humidity sensor
    BOOM_CHANNEL_REF_CAP_HIGH,           // 47 pF reference capacitor
    BOOM_CHANNEL_REF_CAP_LOW,            // 0 pF reference (empty input)
} boom_channel;

/**
 * Measure the oscillator frequency for the given channel.
 *
 * Selects the channel, lets the oscillator settle, then captures 2400
 * consecutive oscillator periods with TIM2 input capture on the MEAS_OUT pin.
 * Interrupts are disabled during the capture window (roughly 25-40 ms for a
 * live channel) to avoid latency jitter. The channel is powered down again
 * before returning.
 *
 * NOTE: TIM2 is reconfigured from scratch, which is safe because the radio
 * data timer (the other TIM2 user) only runs during a transmission and also
 * performs a full reinitialization when it starts.
 *
 * Returns the frequency in Hz, or 0 if the channel does not oscillate
 * (e.g. boom disconnected).
 */
float boom_measure_frequency(boom_channel channel);

/**
 * Drive all boom bias rails and analog switches low, powering the
 * measurement circuit down.
 */
void boom_power_down(void);

#endif
