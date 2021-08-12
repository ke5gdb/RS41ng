#ifndef __CONFIG_INTERNAL_H
#define __CONFIG_INTERNAL_H

#define RADIO_PAYLOAD_MAX_LENGTH 256
#define RADIO_SYMBOL_DATA_MAX_LENGTH 512
#define RADIO_PAYLOAD_MESSAGE_MAX_LENGTH 128

#include <stdbool.h>

extern bool leds_enabled;
extern bool bmp280_enabled;
extern bool si5351_enabled;

extern volatile bool system_initialized;

extern char *aprs_comment_templates[];
extern char *fsq_comment_templates[];
extern char *ftjt_message_templates[];

#endif
