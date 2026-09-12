#include "base64.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "mbedtls/base64.h"

static const char *TAG = "base64";

esp_err_t base64_encode(const uint8_t *src, size_t src_len,
                         uint8_t **dst, size_t *dst_len)
{
    if (!src || !src_len || !dst || !dst_len) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t olen = 0;
    int ret = mbedtls_base64_encode(NULL, 0, &olen, src, src_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        ESP_LOGE(TAG, "mbedtls_base64_encode size query failed: %d", ret);
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)malloc(olen + 1);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes for base64 output", olen);
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_encode(buf, olen, &olen, src, src_len);
    if (ret != 0) {
        ESP_LOGE(TAG, "mbedtls_base64_encode failed: %d", ret);
        free(buf);
        return ESP_FAIL;
    }
    buf[olen] = '\0';

    *dst = buf;
    *dst_len = olen;
    return ESP_OK;
}

void base64_free(uint8_t *buf)
{
    if (buf) {
        free(buf);
    }
}
