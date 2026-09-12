#include "storage_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "profile";

static char s_profile_dir[64] = "/sdcard/profiles";
static char s_default_id[32] = "win11_laptop";
static SemaphoreHandle_t s_mutex = NULL;

/* Single-entry cache for the profile currently in use.
 * The agent sends the active profile on every step, and re-reading it from the
 * SD card each time was pure latency with no benefit: the file only changes on
 * init, on an explicit request_profile switch, or when the model appends a
 * discovery (which invalidates this cache). */
static char *s_cache_id = NULL;
static char *s_cache_content = NULL;

static void cache_invalidate(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    free(s_cache_id);
    free(s_cache_content);
    s_cache_id = NULL;
    s_cache_content = NULL;
    if (s_mutex) xSemaphoreGive(s_mutex);
}

esp_err_t profile_mgr_init(const char *profile_dir, const char *default_id)
{
    if (profile_dir) strncpy(s_profile_dir, profile_dir, sizeof(s_profile_dir) - 1);
    if (default_id) strncpy(s_default_id, default_id, sizeof(s_default_id) - 1);
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    cache_invalidate();
    ESP_LOGI(TAG, "Init: dir=%s default=%s", s_profile_dir, s_default_id);
    return ESP_OK;
}

static char *read_file_content(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc(size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

char *profile_mgr_get(const char *identifier)
{
    const char *id = (identifier && identifier[0]) ? identifier : s_default_id;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_cache_content && s_cache_id && strcmp(s_cache_id, id) == 0) {
        char *cached = strdup(s_cache_content);
        if (s_mutex) xSemaphoreGive(s_mutex);
        if (cached) return cached;
        return strdup("# No profile\n\n(cache allocation failed)");
    }
    if (s_mutex) xSemaphoreGive(s_mutex);

    char path[128];
    snprintf(path, sizeof(path), "%s/%s.md", s_profile_dir, id);
    char *content = read_file_content(path);
    if (!content) {
        ESP_LOGW(TAG, "Profile not found: %s", path);
        return strdup("# No profile\n\nDefault profile not available. Create one at " \
                      "the path above.");
    }

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    char *new_id = strdup(id);
    char *new_content = strdup(content);
    if (new_id && new_content) {
        free(s_cache_id);
        free(s_cache_content);
        s_cache_id = new_id;
        s_cache_content = new_content;
    } else {
        free(new_id);
        free(new_content);
    }
    if (s_mutex) xSemaphoreGive(s_mutex);
    return content;
}

char *profile_mgr_get_default(void)
{
    return profile_mgr_get(s_default_id);
}

/* Largest profile we will grow to. The profile is re-sent on every step of
 * every task, so bloat costs tokens forever. */
#define PROFILE_MAX_BYTES 4096

/*
 * Normalise a line for duplicate detection: lowercase, keep only alphanumeric
 * characters and single spaces. Punctuation and wording differences then stop
 * mattering, which is what catches the model rephrasing the same discovery
 * three times ("Notepad status bar shows line/column and character count",
 * "...shows caret line/column and character count — quick way to verify...").
 */
static void normalize_line(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    bool last_space = true;   /* also trims leading spaces */
    for (const char *p = in; *p && o + 1 < out_len; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[o++] = (char)c;
            last_space = false;
        } else if (!last_space) {
            out[o++] = ' ';
            last_space = true;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = '\0';
}

/* Duplicate detection thresholds. A restatement is accepted as one when the two
 * lines share at least this many distinct words AND that is at least this
 * percentage of the shorter line's words.
 *
 * Both conditions are needed. Pure ratio misses long lines that pad a repeated
 * fact with a unique tail; pure count misses short unrelated lines that happen
 * to share a couple of common words. Measured against the real junk this
 * project produced ("Notepad status bar shows line/column and character count"
 * rephrased three ways), a 70% ratio alone let the second variant through at
 * only ~50%. */
#define PROFILE_DUP_MIN_SHARED  5
#define PROFILE_DUP_RATIO       45

static int count_tokens(const char *s)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", s);
    int n = 0;
    char *save = NULL;
    for (char *w = strtok_r(buf, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (strlen(w) >= 3) n++;
    }
    return n;
}

/* Shared-word count between two normalised lines; *ratio_out is that count as a
 * percentage of the SHORTER line's word count. */
static int line_overlap(const char *a, const char *b, int *ratio_out)
{
    char abuf[256];
    snprintf(abuf, sizeof(abuf), "%s", a);

    int total_a = 0, shared = 0;
    char *save = NULL;
    for (char *w = strtok_r(abuf, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (strlen(w) < 3) continue;   /* ignore "a", "of", "to", ... */
        total_a++;
        if (strstr(b, w) != NULL) shared++;
    }

    int total_b = count_tokens(b);
    int shorter = (total_a < total_b) ? total_a : total_b;
    if (ratio_out) *ratio_out = (shorter > 0) ? (shared * 100 / shorter) : 0;
    return shared;
}

/*
 * True when `line` is already present, or is a near-restatement of something
 * already there. The model reliably re-reports the same discovery when it keeps
 * succeeding at the same subtask; without this the profile fills with
 * paraphrases of one fact.
 */
static bool profile_has_similar_line(const char *existing, const char *line)
{
    if (!existing) return false;

    char norm_new[256];
    normalize_line(line, norm_new, sizeof(norm_new));
    if (!norm_new[0]) return true;            /* nothing but punctuation */

    char *copy = strdup(existing);
    if (!copy) return false;

    bool similar = false;
    char *save = NULL;
    for (char *ln = strtok_r(copy, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        while (*ln == ' ' || *ln == '-' || *ln == '\t') ln++;
        if (!*ln || *ln == '#') continue;

        char norm_old[256];
        normalize_line(ln, norm_old, sizeof(norm_old));
        if (!norm_old[0]) continue;

        if (strcmp(norm_old, norm_new) == 0) {
            ESP_LOGI(TAG, "profile line is an exact duplicate — skipped");
            similar = true;
            break;
        }

        int ratio = 0;
        int shared = line_overlap(norm_old, norm_new, &ratio);
        if (shared >= PROFILE_DUP_MIN_SHARED && ratio >= PROFILE_DUP_RATIO) {
            ESP_LOGI(TAG, "profile line is a restatement (%d shared words, %d%% of "
                          "the shorter line) — skipped", shared, ratio);
            similar = true;
            break;
        }
    }
    free(copy);
    return similar;
}

esp_err_t profile_mgr_append(const char *content, const char *identifier)
{
    if (!content || !content[0]) return ESP_ERR_INVALID_ARG;

    /* An empty identifier must fall back to the default profile, exactly as
     * profile_mgr_get() does. Without the [0] test, an unset active profile
     * produced a file literally named ".md". */
    const char *id = (identifier && identifier[0]) ? identifier : s_default_id;

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    char path[160];
    /* Explicit precision: the identifier is an arbitrary string, so bound it
     * into the buffer rather than relying on silent truncation. */
    snprintf(path, sizeof(path), "%s/%.31s.md", s_profile_dir, id);

    /* Size cap: the profile is sent in full on every step. */
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size >= PROFILE_MAX_BYTES) {
        if (s_mutex) xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Profile %s is full (%ld >= %d bytes) — rejected: %s",
                 path, (long)st.st_size, PROFILE_MAX_BYTES, content);
        return ESP_ERR_NO_MEM;
    }

    /* Deduplicate before writing. */
    char *existing = read_file_content(path);
    if (profile_has_similar_line(existing, content)) {
        free(existing);
        if (s_mutex) xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Profile %s already covers this — skipped: %s", id, content);
        return ESP_OK;   /* not an error: the profile is simply already correct */
    }
    free(existing);

    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGW(TAG, "Cannot append to %s", path);
        if (s_mutex) xSemaphoreGive(s_mutex);
        return ESP_FAIL;
    }
    fprintf(f, "\n- %s\n", content);
    fclose(f);

    /* The file changed, so the cached copy is stale. */
    free(s_cache_id);
    free(s_cache_content);
    s_cache_id = NULL;
    s_cache_content = NULL;

    if (s_mutex) xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Appended to profile %s: %s", id, content);
    return ESP_OK;
}
