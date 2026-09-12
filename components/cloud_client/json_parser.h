#pragma once

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

cJSON *json_parser_parse(const char *raw_text);
void json_parser_set_action_defaults(cJSON *action);
void json_parser_set_response_defaults(cJSON *response);

#ifdef __cplusplus
}
#endif
