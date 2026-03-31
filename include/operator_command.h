#ifndef HIDRELAY_OPERATOR_COMMAND_H
#define HIDRELAY_OPERATOR_COMMAND_H

#include <stdbool.h>

#include "app.h"

bool operator_command_parse_line(
    const char * line,
    const char * token,
    app_operator_command_t * out_command
);

#endif
