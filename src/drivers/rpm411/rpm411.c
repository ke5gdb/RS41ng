#include "config.h"

// The RPM411 pressure module only exists on RS41 hardware (RS41-SGP option)
#if defined(RS41) && SENSOR_RPM411_ENABLE

#include <string.h>
#include <stdio.h>

#include "rpm411.h"
#include "gpio.h"
#include "drivers/hal/spi.h"
#include "drivers/hal/delay.h"
#include "log.h"

#ifndef RS41_RSM4x4
#include <stm32f1xx_hal.h>
#else
#include <stm32l4xx_hal.h>
#endif

/*
 * Protocol notes (reverse engineered from the module firmware disassembly plus
 * on-wire experiments, July 2026):
 *
 * Commands are <opcode...> <crc16 little-endian> <0x00 pad>, and the CRC is
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the opcode bytes.
 * The module echoes every received byte back one byte late, which lets the
 * master verify a command was heard. After a ~450 us processing delay, the
 * response is clocked out as <length> <payload...> <crc16 little-endian>,
 * with the CRC computed over length + payload.
 *
 * After serving a response the module is busy for on the order of 10 ms and
 * ignores the start of the next command (partial or payload-less echo), so
 * every command is retried a few times with a pause in between.
 */

#define RPM411_CMD_MEASURE 0x01
#define RPM411_CMD_READOUT 0x02
#define RPM411_CMD_CONFIG_READ 0x03

// Command length including CRC and trailing pad byte
#define RPM411_MEASURE_COMMAND_LENGTH 5
#define RPM411_CONFIG_COMMAND_LENGTH 7
// Response clock-out lengths follow the original firmware's fixed 33-byte
// exchanges (command bytes + response bytes)
#define RPM411_EXCHANGE_LENGTH 33
#define RPM411_RESPONSE_MAX_LENGTH (RPM411_EXCHANGE_LENGTH - RPM411_MEASURE_COMMAND_LENGTH)

// Conversion time between the measurement trigger and the result readout
#define RPM411_CONVERSION_TIME_MS 250
// Attempts per command: the module misses commands sent while it is busy
#define RPM411_COMMAND_ATTEMPTS 4
// Pause before retrying a command the module did not accept
#define RPM411_BUSY_RETRY_DELAY_MS 20

// Measurement readout payload: 22 bytes, floats at fixed offsets
#define RPM411_MEAS_PAYLOAD_LENGTH 22
#define RPM411_MEAS_OFFSET_TEMPERATURE 14
#define RPM411_MEAS_OFFSET_PRESSURE 18

// Configuration page addresses read by the original firmware at power-up.
// The response to address 0x0028 carries the module serial number.
#define RPM411_SERIAL_ADDRESS 0x0028
static const uint16_t rpm411_config_addresses[] = {
        0x001E, 0x0028, 0x000A, 0x0064, 0x006E, 0x0078, 0x0082,
        0x008C, 0x0096, 0x00A0, 0x00AA, 0x00B4, 0x00BE, 0x00C8,
        0x00D2, 0x00DC, 0x00E6, 0x00F0, 0x00FA, 0x0104, 0x010E,
};
#define RPM411_CONFIG_ADDRESS_COUNT \
    ((int) (sizeof(rpm411_config_addresses) / sizeof(rpm411_config_addresses[0])))

// CRC-16/CCITT-FALSE, transmitted low byte first
static uint16_t rpm411_crc16(const uint8_t *data, int length)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < length; i++) {
        crc ^= (uint16_t) data[i] << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
        }
    }
    return crc;
}

/**
 * Exchange a single byte, framed the way the original firmware does it:
 * chip select toggled around every byte with ~100 us settle delays on both
 * sides (from the logic-analyzer capture).
 */
static uint8_t rpm411_transfer(uint8_t data)
{
    HAL_GPIO_WritePin(BANK_RPM411_CS, PIN_RPM411_CS, GPIO_PIN_RESET);
    delay_us(100);

    uint8_t response = spi_transfer(data);

    HAL_GPIO_WritePin(BANK_RPM411_CS, PIN_RPM411_CS, GPIO_PIN_SET);
    delay_us(100);

    return response;
}

/**
 * Debug aid: dump an exchange as hex on the semihosting log. Compiles to
 * nothing in flight builds.
 */
static void rpm411_log_bytes(const char *tag, int index, const uint8_t *data, int length)
{
#if defined(SEMIHOSTING_ENABLE) && defined(LOGGING_ENABLE)
    char line[3 * RPM411_EXCHANGE_LENGTH + 1];
    if (length > RPM411_EXCHANGE_LENGTH) {
        length = RPM411_EXCHANGE_LENGTH;
    }
    for (int i = 0; i < length; i++) {
        snprintf(&line[3 * i], 4, "%02X ", data[i]);
    }
    log_info("RPM411 %s[%02d]: %s\n", tag, index, line);
#else
    (void) tag;
    (void) index;
    (void) data;
    (void) length;
#endif
}

/**
 * Append the CRC and pad byte to command opcode bytes already placed at the
 * start of the buffer.
 */
static void rpm411_finish_command(uint8_t *command, int opcode_length)
{
    uint16_t crc = rpm411_crc16(command, opcode_length);
    command[opcode_length] = (uint8_t) (crc & 0xFF);
    command[opcode_length + 1] = (uint8_t) (crc >> 8);
    command[opcode_length + 2] = 0x00;
}

/**
 * Send a command and verify that the module echoed it (each RX byte repeats
 * the previous TX byte). A failed echo means the module was busy and never
 * saw the command.
 */
static bool rpm411_send_command(const uint8_t *command, int command_length)
{
    bool echo_ok = true;
    uint8_t previous = 0x00;

    for (int i = 0; i < command_length; i++) {
        uint8_t received = rpm411_transfer(command[i]);
        if (i > 0 && received != previous) {
            echo_ok = false;
        }
        previous = command[i];
    }
    return echo_ok;
}

/**
 * Clock out and validate a response frame: <length> <payload> <crc16>.
 * Returns the payload length, or -1 if the frame is missing or corrupt.
 * The full clocked data is stored in response (response_length bytes).
 */
static int rpm411_receive_response(uint8_t *response, int response_length)
{
    for (int i = 0; i < response_length; i++) {
        response[i] = rpm411_transfer(0x00);
    }

    int payload_length = response[0];
    if (payload_length == 0 || payload_length + 3 > response_length) {
        return -1;
    }

    uint16_t crc = rpm411_crc16(response, payload_length + 1);
    if (response[payload_length + 1] != (uint8_t) (crc & 0xFF)
            || response[payload_length + 2] != (uint8_t) (crc >> 8)) {
        return -1;
    }

    return payload_length;
}

/**
 * Read one configuration page. Returns the payload length (payload copied to
 * payload_out, which must hold RPM411_RESPONSE_MAX_LENGTH bytes), or -1.
 */
static int rpm411_config_read(uint16_t address, uint8_t *payload_out)
{
    uint8_t command[RPM411_CONFIG_COMMAND_LENGTH];
    uint8_t response[RPM411_EXCHANGE_LENGTH - RPM411_CONFIG_COMMAND_LENGTH];

    command[0] = RPM411_CMD_CONFIG_READ;
    command[1] = 0x02;
    command[2] = (uint8_t) (address & 0xFF);
    command[3] = (uint8_t) (address >> 8);
    rpm411_finish_command(command, 4);

    for (int attempt = 0; attempt < RPM411_COMMAND_ATTEMPTS; attempt++) {
        if (attempt > 0) {
            delay_ms(RPM411_BUSY_RETRY_DELAY_MS);
        }

        rpm411_send_command(command, RPM411_CONFIG_COMMAND_LENGTH);
        delay_us(450);

        int payload_length = rpm411_receive_response(response, sizeof(response));
        if (payload_length >= 0) {
            memcpy(payload_out, &response[1], (size_t) payload_length);
            return payload_length;
        }
    }

    rpm411_log_bytes("cfg-fail", address, response, (int) sizeof(response));
    return -1;
}

/**
 * The RPM411 has no local timebase for its measurement engine: the main MCU
 * must supply one on MCO1 / PA8 (labelled RPM_CLOCK in the schematic). Without
 * it the module still answers commands and returns calibration, but every
 * oscillator-channel capture reads zero, so the on-board conversion yields a
 * constant nonsense pressure (~-245 hPa). Runs once; the clock then stays on
 * for the life of the firmware.
 *
 * We source MCO from HSE, the RS41's 24 MHz external oscillator that already
 * drives SYSCLK (so it is always running). This is deliberately NOT the HSI RC
 * oscillator that dspreitz's clean-room reader used: HSE has far lower jitter,
 * which directly reduces the reference-channel noise that the ratiometric
 * pressure calculation otherwise amplifies (~18x). 24 MHz is also almost
 * certainly the clock the original Vaisala main board fed the module on this
 * pin, and the pressure is ratiometric so the exact frequency does not affect
 * the calibrated value.
 *
 * The external-clock dependency itself was found empirically by dspreitz
 * (rs41ng clean-room vaisala_boom); our SPI decompile confirmed the protocol
 * but missed it.
 */
static void rpm411_enable_measurement_clock(void)
{
    static bool enabled = false;
    if (enabled) {
        return;
    }

    // HSE is guaranteed up: it is the active SYSCLK source on RS41 (see
    // system.c), so no enable/ready wait is needed here.
    HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSE, RCC_MCODIV_1);
    enabled = true;
}

bool rpm411_init(char serial[RPM411_SERIAL_LENGTH + 1])
{
    uint8_t payload[RPM411_RESPONSE_MAX_LENGTH];
    int pages_read = 0;

    serial[0] = '\0';

    // Start the module's measurement clock before anything else and let it
    // settle; the oscillator front-end needs it running to produce counts.
    rpm411_enable_measurement_clock();

    delay_ms(50);

    for (int i = 0; i < RPM411_CONFIG_ADDRESS_COUNT; i++) {
        uint16_t address = rpm411_config_addresses[i];

        int payload_length = rpm411_config_read(address, payload);
        if (payload_length < 0) {
            continue;
        }
        pages_read++;

        rpm411_log_bytes("cfg", address, payload, payload_length);

        if (address == RPM411_SERIAL_ADDRESS) {
            int length = 0;
            for (int k = 0; k < payload_length && length < RPM411_SERIAL_LENGTH; k++) {
                char c = (char) payload[k];
                if (c < 0x20 || c > 0x7E) {
                    break;
                }
                serial[length++] = c;
            }
            serial[length] = '\0';
        }

        delay_ms(10);
    }

    if (pages_read < RPM411_CONFIG_ADDRESS_COUNT) {
        log_info("RPM411: %d of %d config pages read\n", pages_read, RPM411_CONFIG_ADDRESS_COUNT);
    }

#if defined(SEMIHOSTING_ENABLE) && defined(LOGGING_ENABLE)
    // Diagnostic: dump the operational measurement-mode parameter IDs (distinct
    // from the factory calibration read above). The module firmware skips every
    // oscillator channel unless these are configured ("Parameter setup not
    // done"); if they read back zero, this module is not commissioned for
    // autonomous measurement. IDs from the firmware parameter table @0x080003f4.
    static const uint16_t op_param_ids[] = {0x0190, 0x019a, 0x01a4, 0x01f4, 0x0258, 0x026c};
    for (unsigned i = 0; i < sizeof(op_param_ids) / sizeof(op_param_ids[0]); i++) {
        int n = rpm411_config_read(op_param_ids[i], payload);
        if (n >= 0) {
            rpm411_log_bytes("op-param", op_param_ids[i], payload, n);
        }
        delay_ms(10);
    }
#endif

    // Require a majority of valid, CRC-checked responses: bus noise cannot
    // fake that, and a missing module yields none at all.
    return pages_read > RPM411_CONFIG_ADDRESS_COUNT / 2;
}

bool rpm411_read(float *pressure_hpa, float *internal_temperature_celsius)
{
    uint8_t command[RPM411_MEASURE_COMMAND_LENGTH];
    uint8_t response[RPM411_EXCHANGE_LENGTH - RPM411_MEASURE_COMMAND_LENGTH];

    // Trigger a measurement; the echo confirms the module accepted it
    command[0] = RPM411_CMD_MEASURE;
    command[1] = 0x00;
    rpm411_finish_command(command, 2);

    bool triggered = false;
    for (int attempt = 0; attempt < RPM411_COMMAND_ATTEMPTS && !triggered; attempt++) {
        if (attempt > 0) {
            delay_ms(RPM411_BUSY_RETRY_DELAY_MS);
        }
        triggered = rpm411_send_command(command, RPM411_MEASURE_COMMAND_LENGTH);
    }
    if (!triggered) {
        log_error("RPM411: measurement trigger not accepted\n");
        return false;
    }

    delay_ms(RPM411_CONVERSION_TIME_MS);

    // Read the result
    command[0] = RPM411_CMD_READOUT;
    command[1] = 0x00;
    rpm411_finish_command(command, 2);

    int payload_length = -1;
    for (int attempt = 0; attempt < RPM411_COMMAND_ATTEMPTS && payload_length < 0; attempt++) {
        if (attempt > 0) {
            delay_ms(RPM411_BUSY_RETRY_DELAY_MS);
        }
        rpm411_send_command(command, RPM411_MEASURE_COMMAND_LENGTH);
        delay_us(450);
        payload_length = rpm411_receive_response(response, sizeof(response));
    }

    rpm411_log_bytes("meas", 0, response, (int) sizeof(response));

    if (payload_length != RPM411_MEAS_PAYLOAD_LENGTH) {
        return false;
    }

    float temperature;
    float pressure;
    memcpy(&temperature, &response[1 + RPM411_MEAS_OFFSET_TEMPERATURE], sizeof(temperature));
    memcpy(&pressure, &response[1 + RPM411_MEAS_OFFSET_PRESSURE], sizeof(pressure));

    // Physical plausibility limits (also a NaN filter: comparisons with NaN
    // are false)
    if (!(pressure > 0.0f && pressure < 1200.0f)
            || !(temperature > -100.0f && temperature < 100.0f)) {
        return false;
    }

    *pressure_hpa = pressure;
    *internal_temperature_celsius = temperature;
    return true;
}

#endif
