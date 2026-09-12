#include "storage_manager.h"
#include "utf8_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "session";

static char s_base_dir[64] = "/sdcard/sessions";
static char s_cur_id[SESSION_ID_LEN] = {0};
static char s_cur_goal[SESSION_GOAL_LEN] = {0};
static uint32_t s_cur_steps = 0;
/* Creation time of the loaded session. Kept alongside the other current-session
 * fields because write_meta() has to re-emit it: it only runs on the session
 * that is loaded, and read_meta() is not called again during a run. */
static char s_cur_created[24] = {0};
static SemaphoreHandle_t s_mutex = NULL;

/* Path buffers are sized for the worst case that can actually occur:
 * base dir (63) + '/' + id (23) + '/' + longest filename (13) + NUL = 101.
 * Kept tight deliberately — session_mgr_create() runs on caller stacks that are
 * not generous (the button task is 6KB) and holds two of these. */
#define SESSION_PATH_MAX 128

/* ── id handling ────────────────────────────────────────────────────── */

/*
 * Validate a caller-supplied session id and copy it into a fixed buffer.
 *
 * Everything that reaches a path is bounded and checked here, which is both the
 * path-traversal guard (no '/', no '.', no '\\') and what lets the path
 * snprintf() calls be provably large enough — the compiler cannot know how long
 * a `const char *` is, so an unbounded "%s" into a fixed buffer is correctly
 * rejected under -Werror=format-truncation.
 */
static bool sanitize_id(const char *id, char out[SESSION_ID_LEN])
{
    if (!id || !id[0]) return false;

    size_t n = strlen(id);
    if (n >= SESSION_ID_LEN) return false;

    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                  (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!ok) return false;
    }

    snprintf(out, SESSION_ID_LEN, "%.*s", SESSION_ID_LEN - 1, id);
    return true;
}

/* ── small file helpers ─────────────────────────────────────────────── */

static char *read_text(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = (sz > 0) ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(text);
    bool ok = (fwrite(text, 1, len, f) == len);
    fclose(f);
    return ok;
}

/* Path of "<base>/<id>" */
static void id_dir(const char *id, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%.*s", s_base_dir, SESSION_ID_LEN - 1, id);
}

/* Path of "<base>/<id>/<name>". Every component carries an explicit precision so
 * the worst case is provable: 63 + 1 + 23 + 1 + 31 + 1 = 120 < SESSION_PATH_MAX. */
static void id_file(const char *id, const char *name, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/%.*s/%.31s",
             s_base_dir, SESSION_ID_LEN - 1, id, name);
}

/* Path of a file inside the CURRENT session directory. */
static void cur_file(const char *name, char *out, size_t out_len)
{
    id_file(s_cur_id, name, out, out_len);
}

/* ── meta.json ──────────────────────────────────────────────────────── */

static void make_title(const char *goal, char *out, size_t out_len)
{
    if (!goal || !goal[0]) {
        snprintf(out, out_len, "(untitled)");
        return;
    }
    /* A byte-wise copy can split a Chinese character in half, and the title ends
     * up in meta.json and in the WebUI session list. */
    utf8_copy(out, out_len, goal);
}

static bool write_meta(void)
{
    char path[SESSION_PATH_MAX];
    cur_file("meta.json", path, sizeof(path));

    cJSON *m = cJSON_CreateObject();
    if (!m) return false;
    cJSON_AddStringToObject(m, "id", s_cur_id);
    char title[SESSION_TITLE_LEN];
    make_title(s_cur_goal, title, sizeof(title));
    cJSON_AddStringToObject(m, "title", title);
    cJSON_AddStringToObject(m, "goal", s_cur_goal);
    cJSON_AddNumberToObject(m, "steps", s_cur_steps);
    /* Re-emit the creation time. Without this, every rewrite of meta.json
     * (session_mgr_set_steps() runs each step) dropped the field, so a loaded
     * session lost the time it was created. */
    if (s_cur_created[0]) {
        cJSON_AddStringToObject(m, "created", s_cur_created);
    }

    char *js = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!js) return false;
    bool ok = write_text(path, js);
    free(js);
    return ok;
}

static bool read_meta(const char *id, session_info_t *info)
{
    char path[SESSION_PATH_MAX];
    id_file(id, "meta.json", path, sizeof(path));

    size_t len = 0;
    char *txt = read_text(path, &len);
    if (!txt) return false;

    cJSON *m = cJSON_Parse(txt);
    free(txt);
    if (!m) return false;

    const cJSON *v;
    memset(info, 0, sizeof(*info));
    /* utf8_copy() rather than "%.*s": these fields are Chinese and a byte-wise
     * cut leaves invalid UTF-8, which the API rejects outright when the goal is
     * echoed back in the next request. */
    if ((v = cJSON_GetObjectItem(m, "id")) && cJSON_IsString(v))
        utf8_copy(info->id, sizeof(info->id), v->valuestring);
    if ((v = cJSON_GetObjectItem(m, "title")) && cJSON_IsString(v)) {
        utf8_copy(info->title, sizeof(info->title), v->valuestring);
        utf8_sanitize(info->title);
    }
    if ((v = cJSON_GetObjectItem(m, "goal")) && cJSON_IsString(v)) {
        utf8_copy(info->goal, sizeof(info->goal), v->valuestring);
        /* meta.json written before the UTF-8 fix can already contain a split
         * character; repair it on the way in so a legacy session is usable. */
        utf8_sanitize(info->goal);
    }
    if ((v = cJSON_GetObjectItem(m, "created")) && cJSON_IsString(v))
        utf8_copy(info->created, sizeof(info->created), v->valuestring);
    if ((v = cJSON_GetObjectItem(m, "steps")) && cJSON_IsNumber(v))
        info->steps = (uint32_t)v->valueint;
    cJSON_Delete(m);

    /* Trajectory size, for the UI. */
    char tp[SESSION_PATH_MAX];
    id_file(id, "trajectory.txt", tp, sizeof(tp));
    struct stat st;
    if (stat(tp, &st) == 0 && st.st_size > 0) info->traj_bytes = (uint32_t)st.st_size;

    return info->id[0] != '\0';
}

/* ── lifecycle ──────────────────────────────────────────────────────── */

esp_err_t session_mgr_init(const char *base_dir)
{
    if (base_dir) {
        /* base + '/' + id must fit plan_mgr's 64-byte path buffer. */
        if (strlen(base_dir) + 1 + SESSION_ID_LEN > 64) {
            ESP_LOGE(TAG, "session dir too long (max %d chars): %s",
                     64 - 1 - SESSION_ID_LEN, base_dir);
            return ESP_ERR_INVALID_ARG;
        }
        strncpy(s_base_dir, base_dir, sizeof(s_base_dir) - 1);
    }
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    mkdir(s_base_dir, 0755);
    ESP_LOGI(TAG, "Init: dir=%s", s_base_dir);
    return ESP_OK;
}

const char *session_mgr_current_id(void)    { return s_cur_id; }
const char *session_mgr_current_goal(void)  { return s_cur_goal; }
uint32_t    session_mgr_current_steps(void) { return s_cur_steps; }

const char *session_mgr_current_dir(void)
{
    static char dir[SESSION_PATH_MAX];
    if (!s_cur_id[0]) { dir[0] = '\0'; return dir; }
    id_dir(s_cur_id, dir, sizeof(dir));
    return dir;
}

int session_mgr_list(session_info_t *out, int max)
{
    if (!out || max <= 0) return 0;

    DIR *d = opendir(s_base_dir);
    if (!d) return 0;

    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (n >= max) break;

        /* Only names that could actually be a session id. d_name can be
         * NAME_MAX long; bounding it here keeps every derived path provably
         * inside its buffer. */
        char id[SESSION_ID_LEN];
        if (!sanitize_id(de->d_name, id)) continue;

        char dir[SESSION_PATH_MAX];
        id_dir(id, dir, sizeof(dir));
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (read_meta(id, &out[n])) {
            if (!out[n].id[0]) {
                utf8_copy(out[n].id, sizeof(out[n].id), id);
            }
            n++;
        }
    }
    closedir(d);

    /* Ids are timestamps (YYYYMMDD_HHMMSS), so a descending sort is
     * newest-first. Simple insertion sort: the list is short. */
    for (int i = 1; i < n; i++) {
        session_info_t key = out[i];
        int j = i - 1;
        while (j >= 0 && strcmp(out[j].id, key.id) < 0) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return n;
}

/* Format "YYYYMMDD_HHMMSS" (plus an optional suffix) into `out`.
 * strftime is used rather than snprintf("%04d%02d...") because the tm fields are
 * plain ints: the compiler cannot prove they stay within 4/2 digits, and
 * -Werror=format-truncation rejects the manual form. */
static bool make_id(char *out, size_t out_len, int suffix)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);

    char base[16];
    if (strftime(base, sizeof(base), "%Y%m%d_%H%M%S", &tmv) == 0) return false;

    if (suffix > 0) {
        snprintf(out, out_len, "%.15s_%02d", base, suffix);
    } else {
        snprintf(out, out_len, "%.15s", base);
    }
    return out[0] != '\0';
}

esp_err_t session_mgr_create(const char *goal, char id_out[SESSION_ID_LEN])
{
    if (!s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    char id[SESSION_ID_LEN];
    char dir[SESSION_PATH_MAX];
    bool free_slot = false;

    /* Two sessions in the same second must not collide. */
    for (int suffix = 0; suffix < 100; suffix++) {
        if (!make_id(id, sizeof(id), suffix)) continue;
        id_dir(id, dir, sizeof(dir));

        struct stat st;
        if (stat(dir, &st) != 0) { free_slot = true; break; }
    }

    if (!free_slot) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Cannot find a free session id");
        return ESP_FAIL;
    }

    if (mkdir(dir, 0755) != 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGE(TAG, "Cannot create session dir %s", dir);
        return ESP_FAIL;
    }

    utf8_copy(s_cur_id, sizeof(s_cur_id), id);
    utf8_copy(s_cur_goal, sizeof(s_cur_goal), goal ? goal : "");
    s_cur_steps = 0;

    /* meta.json, including a human-readable creation time. */
    char path[SESSION_PATH_MAX];
    cur_file("meta.json", path, sizeof(path));

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char created[24];
    if (strftime(created, sizeof(created), "%Y-%m-%d %H:%M:%S", &tmv) == 0) {
        snprintf(created, sizeof(created), "%s", "unknown");
    }
    utf8_copy(s_cur_created, sizeof(s_cur_created), created);

    cJSON *m = cJSON_CreateObject();
    if (m) {
        cJSON_AddStringToObject(m, "id", s_cur_id);
        char title[SESSION_TITLE_LEN];
        make_title(s_cur_goal, title, sizeof(title));
        cJSON_AddStringToObject(m, "title", title);
        cJSON_AddStringToObject(m, "goal", s_cur_goal);
        cJSON_AddStringToObject(m, "created", created);
        cJSON_AddNumberToObject(m, "steps", 0);
        char *js = cJSON_PrintUnformatted(m);
        cJSON_Delete(m);
        if (js) { write_text(path, js); free(js); }
    }

    /* Point the plan manager at this session and start its plan. */
    plan_mgr_init(dir);
    plan_mgr_new_task_with_id(s_cur_id, s_cur_goal);

    if (id_out) utf8_copy(id_out, SESSION_ID_LEN, s_cur_id);
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "New session %s: %s", s_cur_id, s_cur_goal);
    return ESP_OK;
}

esp_err_t session_mgr_load(const char *id)
{
    char sid[SESSION_ID_LEN];
    if (!sanitize_id(id, sid) || !s_mutex) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    session_info_t info;
    if (!read_meta(sid, &info)) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "No such session: %s", sid);
        return ESP_ERR_NOT_FOUND;
    }

    utf8_copy(s_cur_id, sizeof(s_cur_id), info.id);
    utf8_copy(s_cur_goal, sizeof(s_cur_goal), info.goal);
    utf8_copy(s_cur_created, sizeof(s_cur_created), info.created);
    s_cur_steps = info.steps;

    /* Point the plan manager at this session and load its plan. */
    char dir[SESSION_PATH_MAX];
    id_dir(s_cur_id, dir, sizeof(dir));
    plan_mgr_init(dir);
    plan_mgr_load();

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "Loaded session %s (%lu steps): %s",
             s_cur_id, (unsigned long)s_cur_steps, s_cur_goal);
    return ESP_OK;
}

esp_err_t session_mgr_set_steps(uint32_t steps)
{
    if (!s_cur_id[0] || !s_mutex) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cur_steps = steps;
    bool ok = write_meta();
    xSemaphoreGive(s_mutex);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ── trajectory ─────────────────────────────────────────────────────── */

/* Drop the oldest turns, keeping roughly the last SESSION_TRAJ_KEEP bytes and
 * starting at a turn boundary. Trimming happens in one batch when the cap is
 * crossed, not on every append. */
static void trim_trajectory(const char *path)
{
    size_t len = 0;
    char *txt = read_text(path, &len);
    if (!txt) return;
    if (len <= SESSION_TRAJ_KEEP) { free(txt); return; }

    size_t want = len - SESSION_TRAJ_KEEP;
    size_t cut = want;
    while (cut < len && !(txt[cut] == '#' && (cut == 0 || txt[cut - 1] == '\n'))) {
        cut++;
    }
    if (cut >= len) cut = want;   /* no boundary found: hard cut */

    size_t tail = len - cut;
    char *out = malloc(tail + 64);
    if (out) {
        snprintf(out, tail + 64, "[earlier steps trimmed]\n%.*s",
                 (int)tail, txt + cut);
        write_text(path, out);
        ESP_LOGI(TAG, "Trajectory trimmed: %u -> %u bytes",
                 (unsigned)len, (unsigned)strlen(out));
        free(out);
    }
    free(txt);
}

esp_err_t session_mgr_append_turn(const char *record)
{
    if (!s_cur_id[0] || !record || !s_mutex) return ESP_ERR_INVALID_STATE;
    if (!record[0]) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    char path[SESSION_PATH_MAX];
    cur_file("trajectory.txt", path, sizeof(path));

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size >= SESSION_TRAJ_MAX) {
        trim_trajectory(path);
    }

    FILE *f = fopen(path, "ab");
    if (!f) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "Cannot append trajectory: %s", path);
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", record);
    fclose(f);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

char *session_mgr_get_trajectory(void)
{
    if (!s_cur_id[0]) return strdup("(no session loaded)");
    char path[SESSION_PATH_MAX];
    cur_file("trajectory.txt", path, sizeof(path));
    char *txt = read_text(path, NULL);
    return txt ? txt : strdup("(no steps recorded yet)");
}

esp_err_t session_mgr_write_trajectory(const char *text)
{
    if (!s_cur_id[0] || !text) return ESP_ERR_INVALID_STATE;
    if (strlen(text) > SESSION_TRAJ_MAX) return ESP_ERR_INVALID_SIZE;

    char path[SESSION_PATH_MAX];
    cur_file("trajectory.txt", path, sizeof(path));
    return write_text(path, text) ? ESP_OK : ESP_FAIL;
}

/* ── delete / rename ────────────────────────────────────────────────── */

esp_err_t session_mgr_delete(const char *id)
{
    char sid[SESSION_ID_LEN];
    if (!sanitize_id(id, sid)) return ESP_ERR_INVALID_ARG;

    char dir[SESSION_PATH_MAX];
    id_dir(sid, dir, sizeof(dir));

    const char *files[] = { "meta.json", "plan.json", "trajectory.txt" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char p[SESSION_PATH_MAX];
        id_file(sid, files[i], p, sizeof(p));
        remove(p);
    }

    if (rmdir(dir) != 0) {
        ESP_LOGW(TAG, "Cannot remove session dir %s (not empty?)", dir);
        return ESP_FAIL;
    }

    if (strcmp(sid, s_cur_id) == 0) {
        s_cur_id[0] = '\0';
        s_cur_created[0] = '\0';
    }
    ESP_LOGI(TAG, "Deleted session %s", sid);
    return ESP_OK;
}

esp_err_t session_mgr_rename(const char *id, const char *title)
{
    char sid[SESSION_ID_LEN];
    if (!sanitize_id(id, sid)) return ESP_ERR_INVALID_ARG;
    if (!title || !title[0]) return ESP_ERR_INVALID_ARG;

    char path[SESSION_PATH_MAX];
    id_file(sid, "meta.json", path, sizeof(path));

    size_t len = 0;
    char *txt = read_text(path, &len);
    if (!txt) return ESP_ERR_NOT_FOUND;

    cJSON *m = cJSON_Parse(txt);
    free(txt);
    if (!m) return ESP_FAIL;

    char bounded[SESSION_TITLE_LEN];
    utf8_copy(bounded, sizeof(bounded), title);

    cJSON_DeleteItemFromObject(m, "title");
    cJSON_AddStringToObject(m, "title", bounded);
    char *js = cJSON_PrintUnformatted(m);
    cJSON_Delete(m);
    if (!js) return ESP_ERR_NO_MEM;
    bool ok = write_text(path, js);
    free(js);
    return ok ? ESP_OK : ESP_FAIL;
}
