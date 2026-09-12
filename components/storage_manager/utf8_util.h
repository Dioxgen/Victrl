#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UTF-8 safe string handling.
 *
 * Truncating a multi-byte character in half produces invalid UTF-8, and a JSON
 * request body MUST be valid UTF-8 — the API rejects the entire request with a
 * 400. One split Chinese character in the task goal broke every single call
 * while the harness reported the failure as a generic HTTP error.
 *
 * `%.*s` and strncpy() count BYTES, not characters, so any fixed-size copy of
 * user- or model-supplied text (which is routinely Chinese here) can do this.
 * Use these helpers instead.
 */

/* Expected length of the UTF-8 sequence starting with `c`; 0 when `c` cannot
 * start one. */
static inline size_t utf8_seq_len(unsigned char c)
{
    if (c < 0x80)                return 1;
    if ((c & 0xE0) == 0xC0)      return 2;
    if ((c & 0xF0) == 0xE0)      return 3;
    if ((c & 0xF8) == 0xF0)      return 4;
    return 0;
}

/*
 * Copy at most `dst_len - 1` bytes of `src` into `dst`, never splitting a
 * character. The result is always NUL-terminated and always valid UTF-8
 * (assuming the input was).
 */
static inline void utf8_copy(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }

    const unsigned char *p = (const unsigned char *)src;
    const size_t max = dst_len - 1;
    size_t r = 0, w = 0;

    while (p[r] != '\0') {
        size_t need = utf8_seq_len(p[r]);
        if (need == 0) need = 1;          /* invalid lead byte: pass it through */
        if (w + need > max) break;        /* would split a character */
        for (size_t k = 0; k < need; k++) dst[w + k] = (char)p[r + k];
        w += need;
        r += need;
    }
    dst[w] = '\0';
}

/*
 * Rewrite invalid UTF-8 bytes in place as '?', returning how many bytes were
 * replaced (0 means the string was already valid).
 *
 * Replacement is one byte for one byte, so the length never changes — which is
 * why the count, not the length, is the useful signal.
 *
 * The safety net: applied to the finished request body, this guarantees the API
 * never sees a malformed sequence no matter which code path produced one.
 */
static inline size_t utf8_sanitize(char *s)
{
    if (!s) return 0;

    unsigned char *p = (unsigned char *)s;
    size_t r = 0, w = 0, bad = 0;

    while (p[r] != '\0') {
        size_t need = utf8_seq_len(p[r]);
        bool ok = (need > 0);

        if (ok) {
            for (size_t k = 1; k < need; k++) {
                /* Reading the terminator here is fine: it simply fails the
                 * continuation-byte test and ends the sequence. */
                if ((p[r + k] & 0xC0) != 0x80) { ok = false; break; }
            }
        }

        if (ok) {
            for (size_t k = 0; k < need; k++) p[w++] = p[r + k];
            r += need;
        } else {
            p[w++] = '?';
            r += 1;
            bad++;
        }
    }
    p[w] = '\0';
    return bad;
}

#ifdef __cplusplus
}
#endif
