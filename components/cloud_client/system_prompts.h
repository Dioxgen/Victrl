#pragma once

#include <stddef.h>
#include "esp_err.h"

esp_err_t system_prompts_init(const char *sysprompt_dir);
char *system_prompts_get_full(void);
char *system_prompts_get_short(void);
size_t system_prompts_get_full_len(void);
size_t system_prompts_get_short_len(void);
