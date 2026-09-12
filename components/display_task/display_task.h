#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_task_init(void);
void display_task_notify(void);

#ifdef __cplusplus
}
#endif
