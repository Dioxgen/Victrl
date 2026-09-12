#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t base64_encode(const uint8_t *src, size_t src_len,
                         uint8_t **dst, size_t *dst_len);

void base64_free(uint8_t *buf);
