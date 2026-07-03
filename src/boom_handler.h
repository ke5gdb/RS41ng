#ifndef __BOOM_HANDLER_H
#define __BOOM_HANDLER_H

#include "telemetry.h"

bool boom_handler_init();
bool boom_read_telemetry(telemetry_data *data);

#endif
