#include "web_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include "agent_core.h"
#include "storage_manager.h"
#include "utf8_util.h"
#include "wifi_manager.h"
#include "cloud_client.h"
#include "uvc_capture_card_driver.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;
static char s_web_dir[64] = "/sdcard/web";

/* Everything the file API may touch. Any path outside this is refused. */
#define FS_ROOT        "/sdcard"
#define FS_ROOT_LEN    (sizeof(FS_ROOT) - 1)
#define FS_MAX_READ    131072      /* largest file the editor will load      */
#define FS_MAX_WRITE   196608      /* largest body accepted for a write: the
                                    * JSON escaping of a 128KB file can exceed
                                    * the file size itself                    */
#define FS_MAX_ENTRIES 64          /* listing truncates beyond this          */
#define FS_MAX_PATH    256
#define FS_MAX_NAME    256         /* CONFIG_FATFS_MAX_LFN is 255            */

/* ── Helpers ───────────────────────────────────────────────────────── */

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, strlen(json));
}

static esp_err_t send_ok(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t send_error(httpd_req_t *req, httpd_err_code_t code,
                            const char *msg)
{
    const char *status;
    switch (code) {
    case HTTPD_404_NOT_FOUND:               status = "404 Not Found"; break;
    case HTTPD_500_INTERNAL_SERVER_ERROR:   status = "500 Internal Server Error"; break;
    case HTTPD_501_METHOD_NOT_IMPLEMENTED:  status = "501 Not Implemented"; break;
    default:                                status = "400 Bad Request"; break;
    }
    httpd_resp_set_status(req, status);

    char buf[192];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}",
             msg ? msg : "error");
    return send_json(req, buf);
}

/*
 * Read a whole file. Returns NULL only when the file cannot be opened or read;
 * an EMPTY file yields an empty string, not NULL.
 *
 * That distinction matters: a file just created through the WebUI is empty, and
 * treating "empty" as "unreadable" made every freshly created file impossible to
 * open in the editor (the server answered 500 "read failed").
 */
static char *read_file_to_buf(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t rd = (sz > 0) ? fread(buf, 1, (size_t)sz, f) : 0;
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/*
 * Read a request body in full.
 *
 * httpd_req_recv() is allowed to return partial data, and the old /api/start
 * handler read exactly once — so any body longer than its buffer was silently
 * truncated. Writes need the whole thing.
 */
static char *recv_body(httpd_req_t *req, size_t max_len)
{
    size_t total = req->content_len;
    if (total == 0 || total > max_len) {
        ESP_LOGW(TAG, "Body length %u rejected (max %u)",
                 (unsigned)total, (unsigned)max_len);
        return NULL;
    }
    char *buf = malloc(total + 1);
    if (!buf) return NULL;

    size_t got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += (size_t)r;
    }
    buf[total] = '\0';
    return buf;
}

/*
 * Path guard for the SD file API.
 *
 * The WebUI has no authentication, so every path from the browser must be
 * confined to the SD card: reject anything outside FS_ROOT, any ".." component,
 * and the "/sdcardX" prefix trick.
 */
static bool fs_path_ok(const char *path)
{
    if (!path || !path[0]) return false;
    if (strlen(path) >= FS_MAX_PATH) return false;
    if (strncmp(path, FS_ROOT, FS_ROOT_LEN) != 0) return false;

    const char *rest = path + FS_ROOT_LEN;
    if (rest[0] != '\0' && rest[0] != '/') return false;
    if (strstr(path, "..") != NULL) return false;
    return true;
}

/*
 * Percent-decode a query value.
 *
 * httpd_query_key_value() returns the raw, still-encoded value: the browser
 * sends encodeURIComponent("/sdcard/x") as "%2Fsdcard%2Fx", which would never
 * pass fs_path_ok(). Note "+" is deliberately left alone — encodeURIComponent
 * emits %20 for spaces, so treating "+" as a space would corrupt any filename
 * that legitimately contains one.
 */
static void url_decode(char *dst, size_t dst_len, const char *src)
{
    size_t di = 0;
    if (dst_len == 0) return;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_len; si++) {
        if (src[si] == '%' &&
            isxdigit((unsigned char)src[si + 1]) &&
            isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = { src[si + 1], src[si + 2], '\0' };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

/* Extract a URL query parameter ("?path=...") and percent-decode it. */
static bool query_param(httpd_req_t *req, const char *key,
                        char *out, size_t out_len)
{
    char query[512];
    if (httpd_req_get_url_query_len(req) <= 0) return false;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char raw[512];
    if (httpd_query_key_value(query, key, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    url_decode(out, out_len, raw);
    return true;
}

static const char *state_name(agent_state_t s)
{
    switch (s) {
    case AGENT_STATE_RUNNING:   return "RUNNING";
    case AGENT_STATE_PAUSED:    return "PAUSED";
    case AGENT_STATE_STEP:      return "STEP";
    case AGENT_STATE_STOPPING:  return "STOPPING";
    case AGENT_STATE_EMERGENCY: return "EMERGENCY";
    default:                    return "IDLE";
    }
}

/* ── WebUI ─────────────────────────────────────────────────────────── */

static esp_err_t handle_root(httpd_req_t *req)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/index.html", s_web_dir);

    size_t len = 0;
    char *html = read_file_to_buf(path, &len);
    if (!html || len == 0) {
        free(html);
        const char *fallback = "<html><body><h1>Victrl</h1><p>WebUI not found on SD card. Place index.html in /sdcard/web/</p></body></html>";
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, fallback, strlen(fallback));
    }

    httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_send(req, html, len);
    free(html);
    return ret;
}

/* ── /api/status ───────────────────────────────────────────────────── */

static void add_heap(cJSON *parent, const char *name, uint32_t caps)
{
    cJSON *o = cJSON_AddObjectToObject(parent, name);
    cJSON_AddNumberToObject(o, "free", heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(o, "largest", heap_caps_get_largest_free_block(caps));
}

static esp_err_t handle_api_status(httpd_req_t *req)
{
    char ip[32] = "0.0.0.0";
    wifi_manager_get_ip(ip, sizeof(ip));
    bool wifi = wifi_manager_is_connected();

    agent_ctx_t *agent = agent_get_global();
    agent_state_t state = agent ? agent_get_state(agent) : AGENT_STATE_IDLE;

    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");

    /* ── Agent ── */
    cJSON_AddStringToObject(root, "state", state_name(state));
    cJSON_AddNumberToObject(root, "actions", agent ? agent->action_count : 0);
    cJSON_AddNumberToObject(root, "fails", agent ? agent->fail_count : 0);
    cJSON_AddStringToObject(root, "task",
                            (agent && agent->task_goal[0]) ? agent->task_goal : "");
    cJSON_AddStringToObject(root, "profile",
                            (agent && agent->active_profile[0]) ? agent->active_profile : "");
    /* The active conversation window. */
    cJSON_AddStringToObject(root, "session", session_mgr_current_id());
    cJSON_AddStringToObject(root, "session_goal", session_mgr_current_goal());
    cJSON_AddNumberToObject(root, "max_actions", agent ? agent->max_actions : 0);
    cJSON_AddBoolToObject(root, "wait_mode", agent ? agent->wait_mode : false);
    cJSON_AddStringToObject(root, "effort",
                            (agent && agent->last_effort[0]) ? agent->last_effort : "-");
    cJSON_AddNumberToObject(root, "stuck_streak", agent ? agent->stuck_streak : 0);

    /* ── Network ── */
    char mac[24] = "n/a";
    wifi_manager_get_mac(mac, sizeof(mac));
    cJSON *net = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(net, "connected", wifi);
    cJSON_AddStringToObject(net, "ip", ip);
    cJSON_AddNumberToObject(net, "rssi", wifi_manager_get_rssi());
    cJSON_AddStringToObject(net, "mac", mac);

    /* ── Time ── */
    time_status_t ts;
    wifi_manager_get_time_status(&ts);
    cJSON *tm = cJSON_AddObjectToObject(root, "time");
    cJSON_AddBoolToObject(tm, "synced", ts.synced);
    cJSON_AddNumberToObject(tm, "sync_count", ts.sync_count);
    cJSON_AddNumberToObject(tm, "failures", ts.failures);
    cJSON_AddNumberToObject(tm, "servers_configured", ts.n_servers);
    cJSON_AddNumberToObject(tm, "uptime_s", esp_timer_get_time() / 1000000);

    cJSON *srv = cJSON_AddArrayToObject(tm, "servers");
    for (int i = 0; i < ts.n_servers; i++) {
        cJSON_AddItemToArray(srv, cJSON_CreateString(ts.servers[i]));
    }

    time_t now = 0;
    time(&now);
    if (now > 1600000000) {          /* a plausibly-synced clock */
        struct tm lt;
        localtime_r(&now, &lt);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
        cJSON_AddStringToObject(tm, "local", buf);
        cJSON_AddNumberToObject(tm, "epoch", (double)now);
    } else {
        cJSON_AddStringToObject(tm, "local", "not synced");
    }
    if (ts.last_sync_us) {
        cJSON_AddNumberToObject(tm, "since_sync_s",
                                (esp_timer_get_time() - ts.last_sync_us) / 1000000);
    }

    /* ── Memory ── */
    cJSON *heap = cJSON_AddObjectToObject(root, "heap");
    add_heap(heap, "internal", MALLOC_CAP_INTERNAL);
    add_heap(heap, "psram", MALLOC_CAP_SPIRAM);

    /* ── Storage ── */
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(FS_ROOT, &total, &freeb) == ESP_OK) {
        cJSON *fs = cJSON_AddObjectToObject(root, "fs");
        cJSON_AddNumberToObject(fs, "total_kb", (double)(total / 1024));
        cJSON_AddNumberToObject(fs, "free_kb", (double)(freeb / 1024));
    }

    /* ── Last step latency breakdown ──
     * Mirrors the serial "timing(ms)" line so the same numbers can be watched
     * from the browser while a task runs. */
    if (agent && agent->last_timing_step) {
        const step_timing_t *t = &agent->last_timing;
        cJSON *lt = cJSON_AddObjectToObject(root, "last_step");
        cJSON_AddNumberToObject(lt, "step", agent->last_timing_step);
        cJSON_AddNumberToObject(lt, "total_ms", t->total_ms);
        cJSON_AddNumberToObject(lt, "capture_ms", t->capture_ms);
        cJSON_AddNumberToObject(lt, "prep_ms", t->prep_ms);
        cJSON_AddNumberToObject(lt, "jpeg_ms", t->compress_ms);
        cJSON_AddNumberToObject(lt, "jpeg_setup_ms", t->jpeg_setup_ms);
        cJSON_AddNumberToObject(lt, "jpeg_decode_ms", t->jpeg_decode_ms);
        cJSON_AddNumberToObject(lt, "jpeg_resample_ms", t->jpeg_resample_ms);
        cJSON_AddNumberToObject(lt, "jpeg_encode_ms", t->jpeg_encode_ms);
        cJSON_AddNumberToObject(lt, "base64_ms", t->base64_ms);
        cJSON_AddNumberToObject(lt, "json_ms", t->serialize_ms);
        cJSON_AddNumberToObject(lt, "connect_ms", t->connect_ms);
        cJSON_AddNumberToObject(lt, "http_ms", t->http_ms);
        cJSON_AddNumberToObject(lt, "parse_ms", t->parse_ms);
        cJSON_AddNumberToObject(lt, "sdlog_ms", t->log_ms);
        cJSON_AddNumberToObject(lt, "exec_ms", t->exec_ms);
        cJSON_AddNumberToObject(lt, "sleep_ms", t->sleep_ms);
    }

    /* ── Misc ── */
    const char *reset_name;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   reset_name = "POWERON"; break;
    case ESP_RST_SW:        reset_name = "SOFTWARE"; break;
    case ESP_RST_PANIC:     reset_name = "PANIC"; break;
    case ESP_RST_INT_WDT:   reset_name = "INT_WDT"; break;
    case ESP_RST_TASK_WDT:  reset_name = "TASK_WDT"; break;
    case ESP_RST_WDT:       reset_name = "WDT"; break;
    case ESP_RST_BROWNOUT:  reset_name = "BROWNOUT"; break;
    case ESP_RST_DEEPSLEEP: reset_name = "DEEPSLEEP"; break;
    default:                reset_name = "OTHER"; break;
    }
    cJSON_AddStringToObject(root, "reset_reason", reset_name);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    esp_err_t ret = send_json(req, out);
    free(out);
    return ret;
}

/* ── Agent control ─────────────────────────────────────────────────── */

static esp_err_t handle_api_start(httpd_req_t *req)
{
    /* Sized for the JSON envelope, not the task: the task itself is bounded
     * below against SESSION_GOAL_LEN, and JSON escaping can double its length. */
    char *body = recv_body(req, SESSION_GOAL_LEN * 2 + 256);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");

    /* Full SESSION_GOAL_LEN, and utf8_copy rather than strncpy.
     *
     * This used to be `char task[256]` + strncpy(), which silently cut the
     * user's task to 255 bytes — dropping the later steps of a multi-step
     * instruction — and could split a Chinese character doing it. The agent
     * then carried out the truncated task and reported success, which is worse
     * than any crash. Over-long input is now an explicit error. */
    char task[SESSION_GOAL_LEN] = {0};
    char sid[SESSION_ID_LEN] = {0};
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root) {
        const cJSON *t = cJSON_GetObjectItem(root, "task");
        const cJSON *s = cJSON_GetObjectItem(root, "session");
        if (cJSON_IsString(t) && t->valuestring) {
            if (strlen(t->valuestring) >= sizeof(task)) {
                size_t got = strlen(t->valuestring);
                cJSON_Delete(root);
                ESP_LOGW(TAG, "Task rejected: %u bytes, max %u",
                         (unsigned)got, (unsigned)sizeof(task) - 1);
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "task too long: %u bytes, max %u (split it into steps)",
                         (unsigned)got, (unsigned)sizeof(task) - 1);
                return send_error(req, HTTPD_400_BAD_REQUEST, msg);
            }
            utf8_copy(task, sizeof(task), t->valuestring);
        }
        if (cJSON_IsString(s) && s->valuestring) {
            utf8_copy(sid, sizeof(sid), s->valuestring);
        }
        cJSON_Delete(root);
    }

    agent_ctx_t *agent = agent_get_global();
    if (!agent) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "agent not ready");
    }

    esp_err_t err;
    if (sid[0]) {
        /* Continue an existing conversation window. */
        err = agent_start_session(agent, sid);
        if (err == ESP_ERR_NOT_FOUND) {
            return send_error(req, HTTPD_404_NOT_FOUND, "no such session");
        }
    } else if (task[0]) {
        /* Starting a task opens a new window. */
        err = agent_start(agent, task);
    } else {
        return send_error(req, HTTPD_400_BAD_REQUEST, "missing task or session");
    }

    if (err != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "cannot start (not IDLE?)");
    }
    return send_ok(req);
}

static esp_err_t handle_api_stop(httpd_req_t *req)
{
    agent_ctx_t *agent = agent_get_global();
    if (agent) agent_stop(agent);
    return send_ok(req);
}

static esp_err_t handle_api_pause(httpd_req_t *req)
{
    agent_ctx_t *agent = agent_get_global();
    if (agent) agent_pause(agent);
    return send_ok(req);
}

static esp_err_t handle_api_resume(httpd_req_t *req)
{
    agent_ctx_t *agent = agent_get_global();
    if (agent) agent_resume(agent);
    return send_ok(req);
}

static esp_err_t handle_api_step(httpd_req_t *req)
{
    agent_ctx_t *agent = agent_get_global();
    if (agent) agent_step_once(agent);
    return send_ok(req);
}

static esp_err_t handle_api_emergency(httpd_req_t *req)
{
    agent_ctx_t *agent = agent_get_global();
    if (agent) agent_emergency(agent);
    return send_ok(req);
}

/* ── Profiles / plan / log ─────────────────────────────────────────── */

static esp_err_t handle_api_profiles(httpd_req_t *req)
{
    agent_ctx_t *agent = agent_get_global();
    const char *active = (agent && agent->active_profile[0])
                             ? agent->active_profile : "none";

    char list[512];
    snprintf(list, sizeof(list),
        "{\"active\":\"%s\",\"profiles\":[]}", active);
    return send_json(req, list);
}

static esp_err_t handle_api_plan(httpd_req_t *req)
{
    cJSON *plan = plan_mgr_get_current();
    if (!plan) {
        return send_json(req, "{\"summary\":\"No plan\",\"milestones\":[]}");
    }
    char *s = cJSON_PrintUnformatted(plan);
    cJSON_Delete(plan);
    if (!s) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    esp_err_t ret = send_json(req, s);
    free(s);
    return ret;
}

static esp_err_t handle_api_log(httpd_req_t *req)
{
    char *history = stm_get_all_formatted();
    agent_ctx_t *agent = agent_get_global();
    agent_state_t state = agent ? agent_get_state(agent) : AGENT_STATE_IDLE;

    const char *goal = (agent && agent->task_goal[0]) ? agent->task_goal : "";
    size_t goal_len = strlen(goal);
    size_t hist_len = history ? strlen(history) : 0;
    /* The task goal is echoed verbatim, so it must be sized for and JSON-escaped
     * like the history is. A goal containing quotes (tasks that spell out a JSON
     * payload do) otherwise produced a response the browser could not parse. */
    size_t buf_size = hist_len * 2 + goal_len * 2 + 512;

    char *buf = (char *)malloc(buf_size);
    if (!buf) {
        free(history);
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    size_t off = (size_t)snprintf(buf, buf_size,
        "{\"state\":\"%s\",\"step\":%lu,\"task\":\"",
        state_name(state),
        agent ? agent->action_count : 0);

    for (const char *p = goal; *p && off + 8 < buf_size; p++) {
        if (*p == '\n')      { buf[off++] = '\\'; buf[off++] = 'n'; }
        else if (*p == '"')  { buf[off++] = '\\'; buf[off++] = '"'; }
        else if (*p == '\\') { buf[off++] = '\\'; buf[off++] = '\\'; }
        else                   buf[off++] = *p;
    }

    if (off + 8 < buf_size) off += (size_t)snprintf(buf + off, buf_size - off, "\",\"log\":\"");

    if (history) {
        for (char *p = history; *p && off + 8 < buf_size; p++) {
            if (*p == '\n') { buf[off++] = '\\'; buf[off++] = 'n'; }
            else if (*p == '"') { buf[off++] = '\\'; buf[off++] = '"'; }
            else if (*p == '\\') { buf[off++] = '\\'; buf[off++] = '\\'; }
            else buf[off++] = *p;
        }
        free(history);
    }
    if (off + 4 < buf_size) off += (size_t)snprintf(buf + off, buf_size - off, "\"}");
    else buf[off < buf_size ? off : buf_size - 1] = '\0';

    esp_err_t ret = send_json(req, buf);
    free(buf);
    return ret;
}

/* ── SD card file API ──────────────────────────────────────────────── */

typedef struct {
    char name[FS_MAX_NAME];
    bool is_dir;
    long size;
} fs_entry_t;

static int fs_entry_cmp(const void *a, const void *b)
{
    const fs_entry_t *x = (const fs_entry_t *)a;
    const fs_entry_t *y = (const fs_entry_t *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;   /* dirs first */
    return strcasecmp(x->name, y->name);
}

static esp_err_t handle_api_fs_list(httpd_req_t *req)
{
    char path[FS_MAX_PATH];
    if (!query_param(req, "path", path, sizeof(path))) {
        snprintf(path, sizeof(path), "%s", FS_ROOT);
    }
    if (!fs_path_ok(path)) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "path outside /sdcard");
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return send_error(req, HTTPD_404_NOT_FOUND, "no such directory");
    }
    if (!S_ISDIR(st.st_mode)) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "not a directory");
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "cannot open directory");
    }

    fs_entry_t *entries = calloc(FS_MAX_ENTRIES, sizeof(fs_entry_t));
    if (!entries) {
        closedir(dir);
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }

    int n = 0;
    bool truncated = false;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (n >= FS_MAX_ENTRIES) { truncated = true; break; }

        char full[FS_MAX_PATH + FS_MAX_NAME + 8];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        /* d_type is not reliable on FATFS — stat the entry instead. */
        struct stat est;
        if (stat(full, &est) != 0) continue;

        snprintf(entries[n].name, sizeof(entries[n].name), "%s", de->d_name);
        entries[n].is_dir = S_ISDIR(est.st_mode);
        entries[n].size = (long)est.st_size;
        n++;
    }
    closedir(dir);

    if (n > 1) qsort(entries, n, sizeof(fs_entry_t), fs_entry_cmp);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(entries);
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    }
    cJSON_AddStringToObject(root, "path", path);
    cJSON_AddBoolToObject(root, "truncated", truncated);
    cJSON *arr = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", entries[i].name);
        cJSON_AddStringToObject(e, "type", entries[i].is_dir ? "dir" : "file");
        cJSON_AddNumberToObject(e, "size", (double)entries[i].size);
        cJSON_AddItemToArray(arr, e);
    }
    free(entries);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    esp_err_t ret = send_json(req, out);
    free(out);
    return ret;
}

static esp_err_t handle_api_fs_read(httpd_req_t *req)
{
    char path[FS_MAX_PATH];
    if (!query_param(req, "path", path, sizeof(path)) || !fs_path_ok(path)) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }

    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return send_error(req, HTTPD_404_NOT_FOUND, "no such file");
    }
    if (st.st_size > FS_MAX_READ) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "file too large for the editor");
    }

    size_t len = 0;
    char *content = read_file_to_buf(path, &len);
    if (!content) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "read failed");
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, content, len);
    free(content);
    return ret;
}

static esp_err_t fs_write_handler(httpd_req_t *req)
{
    char *body = recv_body(req, FS_MAX_WRITE);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, HTTPD_400_BAD_REQUEST, "invalid json");

    const cJSON *jp = cJSON_GetObjectItem(root, "path");
    const cJSON *jc = cJSON_GetObjectItem(root, "content");
    if (!cJSON_IsString(jp) || !fs_path_ok(jp->valuestring)) {
        cJSON_Delete(root);
        return send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    const char *content = cJSON_IsString(jc) ? jc->valuestring : "";

    FILE *f = fopen(jp->valuestring, "wb");
    if (!f) {
        cJSON_Delete(root);
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot open for write");
    }
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    char msg[160];
    snprintf(msg, sizeof(msg), "wrote %u of %u bytes",
             (unsigned)written, (unsigned)len);
    bool ok = (written == len);
    cJSON_Delete(root);

    if (!ok) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "short write");
    ESP_LOGI(TAG, "fs write: %s", msg);
    return send_ok(req);
}

static esp_err_t handle_api_fs_delete(httpd_req_t *req)
{
    char *body = recv_body(req, 1024);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, HTTPD_400_BAD_REQUEST, "invalid json");

    const cJSON *jp = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(jp) || !fs_path_ok(jp->valuestring) ||
        strcmp(jp->valuestring, FS_ROOT) == 0) {
        cJSON_Delete(root);
        return send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    char path[FS_MAX_PATH];
    snprintf(path, sizeof(path), "%s", jp->valuestring);
    cJSON_Delete(root);

    /* remove() handles files; empty directories need rmdir(). */
    if (remove(path) != 0) {
        if (rmdir(path) != 0) {
            return send_error(req, HTTPD_400_BAD_REQUEST,
                              "delete failed (directory not empty?)");
        }
    }
    ESP_LOGI(TAG, "fs delete: %s", path);
    return send_ok(req);
}

static esp_err_t handle_api_fs_mkdir(httpd_req_t *req)
{
    char *body = recv_body(req, 1024);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, HTTPD_400_BAD_REQUEST, "invalid json");

    const cJSON *jp = cJSON_GetObjectItem(root, "path");
    if (!cJSON_IsString(jp) || !fs_path_ok(jp->valuestring)) {
        cJSON_Delete(root);
        return send_error(req, HTTPD_400_BAD_REQUEST, "bad path");
    }
    char path[FS_MAX_PATH];
    snprintf(path, sizeof(path), "%s", jp->valuestring);
    cJSON_Delete(root);

    if (mkdir(path, 0755) != 0) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "mkdir failed (exists?)");
    }
    ESP_LOGI(TAG, "fs mkdir: %s", path);
    return send_ok(req);
}

static esp_err_t handle_api_reboot(httpd_req_t *req)
{
    /* Answer first, then restart — otherwise the browser sees a dead socket
     * instead of confirmation. */
    esp_err_t ret = send_json(req, "{\"ok\":true,\"rebooting\":true}");
    ESP_LOGW(TAG, "Reboot requested from WebUI");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ret;   /* not reached */
}

/* ── Sessions (conversation windows) ───────────────────────────────── */

#define SESSION_LIST_MAX 24

static esp_err_t handle_api_sessions(httpd_req_t *req)
{
    /* Heap, not stack: session_info_t is ~300 bytes and the httpd task has 8KB. */
    session_info_t *list = calloc(SESSION_LIST_MAX, sizeof(session_info_t));
    if (!list) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    int n = session_mgr_list(list, SESSION_LIST_MAX);

    cJSON *root = cJSON_CreateObject();
    if (!root) { free(list); return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); }

    cJSON_AddStringToObject(root, "current", session_mgr_current_id());
    cJSON_AddStringToObject(root, "current_goal", session_mgr_current_goal());
    cJSON_AddNumberToObject(root, "current_steps", session_mgr_current_steps());

    cJSON *arr = cJSON_AddArrayToObject(root, "sessions");
    for (int i = 0; i < n; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "id", list[i].id);
        cJSON_AddStringToObject(s, "title", list[i].title);
        /* Bound the goal here: SESSION_GOAL_LEN is 1KB, and the list would
         * otherwise return up to 24KB of goal text on every poll. The WebUI
         * only uses this as a fallback label for rows with no title. */
        char goal[160];
        utf8_copy(goal, sizeof(goal), list[i].goal);
        cJSON_AddStringToObject(s, "goal", goal);
        cJSON_AddStringToObject(s, "created", list[i].created);
        cJSON_AddNumberToObject(s, "steps", list[i].steps);
        cJSON_AddNumberToObject(s, "traj_bytes", list[i].traj_bytes);
        cJSON_AddItemToArray(arr, s);
    }
    free(list);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    esp_err_t ret = send_json(req, out);
    free(out);
    return ret;
}

/* Load the newest remaining session, or create a placeholder window. */
static void session_fallback_current(void)
{
    session_info_t one;
    if (session_mgr_list(&one, 1) > 0) {
        session_mgr_load(one.id);
    } else {
        session_mgr_create("Waiting for task assignment...", NULL);
    }
}

static esp_err_t handle_api_session_select(httpd_req_t *req)
{
    char *body = recv_body(req, 512);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, HTTPD_400_BAD_REQUEST, "invalid json");

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    char sid[SESSION_ID_LEN] = {0};
    if (cJSON_IsString(id) && id->valuestring) {
        snprintf(sid, sizeof(sid), "%.*s", SESSION_ID_LEN - 1, id->valuestring);
    }
    cJSON_Delete(root);

    if (!sid[0]) return send_error(req, HTTPD_400_BAD_REQUEST, "missing id");

    /* Switching the active context while the agent is running would swap the
     * goal and plan out from under it. */
    agent_ctx_t *agent = agent_get_global();
    if (agent && agent_get_state(agent) != AGENT_STATE_IDLE) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "stop the agent before switching");
    }

    if (session_mgr_load(sid) != ESP_OK) {
        return send_error(req, HTTPD_404_NOT_FOUND, "no such session");
    }
    return send_ok(req);
}

static esp_err_t handle_api_session_delete(httpd_req_t *req)
{
    char *body = recv_body(req, 512);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, HTTPD_400_BAD_REQUEST, "invalid json");

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    char sid[SESSION_ID_LEN] = {0};
    if (cJSON_IsString(id) && id->valuestring) {
        snprintf(sid, sizeof(sid), "%.*s", SESSION_ID_LEN - 1, id->valuestring);
    }
    cJSON_Delete(root);

    if (!sid[0]) return send_error(req, HTTPD_400_BAD_REQUEST, "missing id");

    agent_ctx_t *agent = agent_get_global();
    if (agent && agent_get_state(agent) != AGENT_STATE_IDLE) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "stop the agent first");
    }

    bool was_current = (strcmp(sid, session_mgr_current_id()) == 0);
    if (session_mgr_delete(sid) != ESP_OK) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "delete failed");
    }
    if (was_current) session_fallback_current();
    return send_ok(req);
}

static esp_err_t handle_api_session_rename(httpd_req_t *req)
{
    char *body = recv_body(req, 512);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, HTTPD_400_BAD_REQUEST, "invalid json");

    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *ti = cJSON_GetObjectItem(root, "title");
    char sid[SESSION_ID_LEN] = {0};
    char title[SESSION_TITLE_LEN] = {0};
    if (cJSON_IsString(id) && id->valuestring)
        snprintf(sid, sizeof(sid), "%.*s", SESSION_ID_LEN - 1, id->valuestring);
    if (cJSON_IsString(ti) && ti->valuestring)
        utf8_copy(title, sizeof(title), ti->valuestring);
    cJSON_Delete(root);

    if (!sid[0] || !title[0]) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "missing id or title");
    }
    if (session_mgr_rename(sid, title) != ESP_OK) {
        return send_error(req, HTTPD_404_NOT_FOUND, "rename failed");
    }
    return send_ok(req);
}

static esp_err_t handle_api_session_trajectory_get(httpd_req_t *req)
{
    char *txt = session_mgr_get_trajectory();
    if (!txt) return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, txt, strlen(txt));
    free(txt);
    return ret;
}

static esp_err_t handle_api_session_trajectory_set(httpd_req_t *req)
{
    char *body = recv_body(req, FS_MAX_WRITE);
    if (!body) return send_error(req, HTTPD_400_BAD_REQUEST, "bad body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return send_error(req, HTTPD_400_BAD_REQUEST, "invalid json");

    const cJSON *c = cJSON_GetObjectItem(root, "content");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(c)) {
        err = session_mgr_write_trajectory(c->valuestring);
    }
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_SIZE) {
        return send_error(req, HTTPD_400_BAD_REQUEST, "trajectory too long");
    }
    if (err != ESP_OK) {
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
    }
    return send_ok(req);
}

/* ── /api/snapshot ─────────────────────────────────────────────────── */

/*
 * What the model sees.
 *
 *   GET /api/snapshot          -> the exact image last sent to the model
 *                                 (post crop/scale), i.e. its field of view
 *   GET /api/snapshot?live=1   -> a fresh frame straight from the capture card
 *
 * The first is the diagnostic for the ROI feature: it is the only way to check
 * that a crop covers what you expect. The second shows what the camera sees
 * regardless of what the agent is doing.
 */
static esp_err_t handle_api_snapshot(httpd_req_t *req)
{
    bool live = false;
    char query[64];
    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[8];
        if (httpd_query_key_value(query, "live", v, sizeof(v)) == ESP_OK &&
            v[0] == '1') {
            live = true;
        }
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!live) {
        size_t len = 0;
        const uint8_t *jpg = cloud_client_get_last_jpeg(&len);
        if (!jpg || len == 0) {
            return send_error(req, HTTPD_404_NOT_FOUND,
                              "nothing has been sent to the model yet");
        }
        return httpd_resp_send(req, (const char *)jpg, len);
    }

    /* Live: grab a frame. Short timeout so a dead stream cannot stall the
     * web server for seconds. */
    uint8_t *frame = NULL;
    size_t frame_len = 0;
    esp_err_t err = uvc_capture_one_frame(&frame, &frame_len, 2000);
    if (err != ESP_OK || !frame || frame_len == 0) {
        free(frame);
        return send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                          "capture failed (is the capture card connected?)");
    }
    esp_err_t ret = httpd_resp_send(req, (const char *)frame, frame_len);
    free(frame);
    return ret;
}

/* ── Start / Stop ──────────────────────────────────────────────────── */

esp_err_t web_server_start(uint16_t port, const char *web_dir)
{
    if (web_dir) strncpy(s_web_dir, web_dir, sizeof(s_web_dir) - 1);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_uri_handlers = 32;
    /* The file API does FATFS + cJSON work inline; the 4KB default stack is
     * uncomfortably close for a directory listing. */
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", err);
        return err;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/api/status",      .method = HTTP_GET,  .handler = handle_api_status },
        { .uri = "/api/start",       .method = HTTP_POST, .handler = handle_api_start },
        { .uri = "/api/stop",        .method = HTTP_POST, .handler = handle_api_stop },
        { .uri = "/api/pause",       .method = HTTP_POST, .handler = handle_api_pause },
        { .uri = "/api/resume",      .method = HTTP_POST, .handler = handle_api_resume },
        { .uri = "/api/step",        .method = HTTP_POST, .handler = handle_api_step },
        { .uri = "/api/emergency",   .method = HTTP_POST, .handler = handle_api_emergency },
        { .uri = "/api/profiles",    .method = HTTP_GET,  .handler = handle_api_profiles },
        { .uri = "/api/plan",        .method = HTTP_GET,  .handler = handle_api_plan },
        { .uri = "/api/log",         .method = HTTP_GET,  .handler = handle_api_log },
        { .uri = "/api/fs",          .method = HTTP_GET,  .handler = handle_api_fs_list },
        { .uri = "/api/fs/read",     .method = HTTP_GET,  .handler = handle_api_fs_read },
        { .uri = "/api/fs/write",    .method = HTTP_POST, .handler = fs_write_handler },
        { .uri = "/api/fs/delete",   .method = HTTP_POST, .handler = handle_api_fs_delete },
        { .uri = "/api/fs/mkdir",    .method = HTTP_POST, .handler = handle_api_fs_mkdir },
        { .uri = "/api/reboot",      .method = HTTP_POST, .handler = handle_api_reboot },
        { .uri = "/api/sessions",              .method = HTTP_GET,  .handler = handle_api_sessions },
        { .uri = "/api/sessions/select",       .method = HTTP_POST, .handler = handle_api_session_select },
        { .uri = "/api/sessions/delete",       .method = HTTP_POST, .handler = handle_api_session_delete },
        { .uri = "/api/sessions/rename",       .method = HTTP_POST, .handler = handle_api_session_rename },
        { .uri = "/api/sessions/trajectory",   .method = HTTP_GET,  .handler = handle_api_session_trajectory_get },
        { .uri = "/api/sessions/trajectory",   .method = HTTP_POST, .handler = handle_api_session_trajectory_set },
        { .uri = "/api/snapshot",              .method = HTTP_GET,  .handler = handle_api_snapshot },
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t r = httpd_register_uri_handler(s_server, &routes[i]);
        if (r != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register %s: %d", routes[i].uri, r);
        }
    }

    ESP_LOGI(TAG, "HTTP server started on port %u (%u routes)",
             port, (unsigned)(sizeof(routes) / sizeof(routes[0])));
    return ESP_OK;
}

esp_err_t web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    return ESP_OK;
}
