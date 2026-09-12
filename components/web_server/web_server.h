#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(uint16_t port, const char *web_dir);
esp_err_t web_server_stop(void);

#ifdef __cplusplus
}
#endif
