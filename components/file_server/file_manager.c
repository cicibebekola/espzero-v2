#include "file_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_CAP 240
#define FULL_CAP 384
#define IO_BUFFER_SIZE 2048
/* Only the guarded HTTP handler accesses this workspace. Storage ownership
 * serializes requests; keep large parser/transfer arrays off the task stack. */
typedef struct {
    char query[1600];
    char encoded[PATH_CAP * 3];
    char io[IO_BUFFER_SIZE];
} request_workspace_t;
static request_workspace_t *workspace;
static char mount[64];
static file_server_transfer_cb_t notify;
extern const unsigned char page_start[] asm("_binary_index_html_start");
extern const unsigned char page_end[] asm("_binary_index_html_end");

static esp_err_t error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, message, HTTPD_RESP_USE_STRLEN);
}

/* FAT accepts 8.3 aliases for hidden long names. Only allow existing entries
 * by their displayed long names, never by aliases to private system files. */
static bool visible_path(const char *path)
{
    char directory[FULL_CAP];
    snprintf(directory, sizeof(directory), "%s", mount);
    const char *component = path + 1;
    while (*component) {
        const char *slash = strchr(component, '/');
        size_t length = slash ? (size_t)(slash-component) : strlen(component);
        DIR *dir = opendir(directory);
        if (!dir) return true; /* Let the operation report the storage error. */
        bool found = false; struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (entry->d_name[0] != '.' && strlen(entry->d_name) == length &&
                strncasecmp(entry->d_name, component, length) == 0) { found = true; break; }
        }
        closedir(dir);
        size_t used = strlen(directory);
        if (used + length + 2 > sizeof(directory)) return false;
        directory[used++] = '/';
        memcpy(directory + used, component, length); directory[used+length] = 0;
        if (!found) {
            struct stat st;
            if (stat(directory, &st) == 0) return false; /* An alias, not a visible name. */
        }
        if (!slash) break;
        component = slash + 1;
    }
    return true;
}

/* Decode once, then validate every FAT path component. Hidden system files
 * (including the credential backup and upload staging files) are private. */
static bool parameter(httpd_req_t *req, const char *key, char out[PATH_CAP])
{
    char *query = workspace->query, *encoded = workspace->encoded;
    if (httpd_req_get_url_query_str(req, query, sizeof(workspace->query)) != ESP_OK ||
        httpd_query_key_value(query, key, encoded, sizeof(workspace->encoded)) != ESP_OK) return false;
    size_t n = 0;
    for (size_t i = 0; encoded[i]; ++i) {
        unsigned char c = encoded[i];
        if (c == '%') {
            if (!encoded[i+1] || !encoded[i+2] || !isxdigit((unsigned char)encoded[i+1]) ||
                !isxdigit((unsigned char)encoded[i+2])) return false;
            char hex[] = {encoded[i+1], encoded[i+2], 0};
            c = strtoul(hex, NULL, 16); i += 2;
        } else if (c == '+') c = ' ';
        if (n + 1 >= PATH_CAP || c < 32 || c == 127 || strchr("\\:*?\"<>|", c)) return false;
        out[n++] = c;
    }
    out[n] = 0;
    if (n == 0 || out[0] != '/') return false;
    if (n == 1) return true;
    const char *component = out + 1;
    for (const char *p = component; ; ++p) {
        if (*p == '/' || !*p) {
            if (p == component || *component == '.' || p[-1] == '.' || p[-1] == ' ') return false;
            if (!*p) break;
            component = p + 1;
        }
    }
    return visible_path(out);
}

static void full_path(const char *path, char full[FULL_CAP])
{
    snprintf(full, FULL_CAP, "%s%s", mount, path);
}

static esp_err_t fs_error(httpd_req_t *req)
{
    if (errno == ENOENT) return error(req, "404 Not Found", "File or folder not found. Is the SD card inserted?");
    if (errno == EEXIST) return error(req, "409 Conflict", "That name already exists. Rename it or choose another name.");
    if (errno == ENOSPC) return error(req, "507 Insufficient Storage", "The SD card is full.");
    return error(req, "500 Internal Server Error", "SD card operation failed. Check the card and try again.");
}

static esp_err_t root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)page_start, page_end - page_start - 1);
}

static esp_err_t json_string(httpd_req_t *req, const char *value)
{
    char escaped[PATH_CAP * 6 + 3]; size_t n = 0;
    escaped[n++] = '"';
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { escaped[n++] = '\\'; escaped[n++] = *p; }
        else if (*p < 32) n += snprintf(escaped+n, sizeof(escaped)-n, "\\u%04x", *p);
        else escaped[n++] = *p;
    }
    escaped[n++] = '"';
    return httpd_resp_send_chunk(req, escaped, n);
}

static esp_err_t list(httpd_req_t *req)
{
    char path[PATH_CAP], full[FULL_CAP];
    if (!parameter(req, "path", path)) return error(req, "400 Bad Request", "Invalid folder path.");
    full_path(path, full);
    DIR *dir = opendir(full);
    if (!dir) return fs_error(req);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send_chunk(req, "[", 1);
    struct dirent *entry; bool first = true;
    while (result == ESP_OK && (entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char child[FULL_CAP]; struct stat st;
        int len = snprintf(child, sizeof(child), "%s%s%s", full,
                           full[strlen(full)-1] == '/' ? "" : "/", entry->d_name);
        if (len >= sizeof(child) || stat(child, &st) != 0) continue;
        result = httpd_resp_send_chunk(req, first ? "{\"name\":" : ",{\"name\":", HTTPD_RESP_USE_STRLEN);
        first = false;
        if (result == ESP_OK) result = json_string(req, entry->d_name);
        char suffix[120];
        snprintf(suffix, sizeof(suffix), ",\"directory\":%s,\"size\":%lld,\"modified\":%lld}",
                 S_ISDIR(st.st_mode) ? "true" : "false", (long long)st.st_size, (long long)st.st_mtime);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, suffix, HTTPD_RESP_USE_STRLEN);
    }
    closedir(dir);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, "]", 1);
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, NULL, 0);
    return result;
}

static void event(const char *operation, const char *path, size_t done, size_t total, bool finished, bool ok)
{
    if (notify) notify(operation, path, done, total, finished, ok);
}

static esp_err_t upload(httpd_req_t *req)
{
    char path[PATH_CAP], full[FULL_CAP], staging[FULL_CAP]; struct stat st;
    if (!parameter(req, "path", path) || !strcmp(path, "/")) return error(req, "400 Bad Request", "Choose a file name and destination folder.");
    full_path(path, full);
    if (stat(full, &st) == 0) return error(req, "409 Conflict", "That name already exists. Existing files are never overwritten automatically.");
    if (errno != ENOENT) return fs_error(req);
    /* Same-directory staging keeps incomplete content out of the app browser. */
    strcpy(staging, full);
    char *base = strrchr(staging, '/');
    strcpy(base + 1, ".espzero-upload.tmp");
    FILE *file = fopen(staging, "wb");
    if (!file) return fs_error(req);
    size_t done = 0, total = req->content_len;
    int timeouts = 0, last = -1; bool ok = true;
    event("Upload", path, 0, total, false, true);
    char *buffer = workspace->io;
    while (done < total) {
        int received = httpd_req_recv(req, buffer, total-done < IO_BUFFER_SIZE ? total-done : IO_BUFFER_SIZE);
        if (received == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
        if (received <= 0 || fwrite(buffer, 1, received, file) != received) { ok = false; break; }
        timeouts = 0; done += received;
        int percent = (int)((uint64_t)done * 100 / total);
        if (percent != last) { event("Upload", path, done, total, false, true); last = percent; }
    }
    if (fclose(file) != 0) ok = false;
    if (ok && rename(staging, full) != 0) ok = false;
    if (!ok) unlink(staging);
    event("Upload", path, done, total, true, ok);
    if (!ok) return error(req, "500 Internal Server Error", "Upload failed. Check free space and the SD card; incomplete file removed.");
    return httpd_resp_send(req, "Uploaded", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t download(httpd_req_t *req)
{
    char path[PATH_CAP], full[FULL_CAP]; struct stat st;
    if (!parameter(req, "path", path)) return error(req, "400 Bad Request", "Invalid file path.");
    full_path(path, full);
    if (stat(full, &st)) return fs_error(req);
    if (!S_ISREG(st.st_mode)) return error(req, "400 Bad Request", "Open the folder to download its files.");
    FILE *file = fopen(full, "rb");
    if (!file) return fs_error(req);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char *buffer = workspace->io; size_t count, done = 0; int last = -1;
    esp_err_t result = ESP_OK;
    event("Download", path, 0, st.st_size, false, true);
    while ((count = fread(buffer, 1, IO_BUFFER_SIZE, file)) > 0) {
        result = httpd_resp_send_chunk(req, buffer, count);
        if (result != ESP_OK) break;
        done += count;
        int percent = (int)((uint64_t)done * 100 / (st.st_size ? st.st_size : 1));
        if (percent != last) { event("Download", path, done, st.st_size, false, true); last = percent; }
    }
    bool ok = result == ESP_OK && !ferror(file) && done == st.st_size;
    fclose(file);
    if (ok) result = httpd_resp_send_chunk(req, NULL, 0);
    event("Download", path, done, st.st_size, true, ok && result == ESP_OK);
    return ok ? result : ESP_FAIL;
}

static esp_err_t mkdir_handler(httpd_req_t *req)
{
    char path[PATH_CAP], full[FULL_CAP];
    if (!parameter(req, "path", path) || !strcmp(path, "/")) return error(req, "400 Bad Request", "Invalid new folder path.");
    full_path(path, full);
    if (mkdir(full, 0775)) return fs_error(req);
    event("New folder", path, 0, 0, true, true);
    return httpd_resp_send(req, "Folder created", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t move_handler(httpd_req_t *req)
{
    char source[PATH_CAP], target[PATH_CAP], from[FULL_CAP], to[FULL_CAP]; struct stat st;
    if (!parameter(req, "path", source) || !parameter(req, "to", target) ||
        !strcmp(source, "/") || !strcmp(target, "/")) return error(req, "400 Bad Request", "Invalid source or destination.");
    size_t len = strlen(source);
    if (!strncmp(source, target, len) && (target[len] == '/' || !target[len]))
        return error(req, "400 Bad Request", "Cannot move a folder into itself.");
    full_path(source, from); full_path(target, to);
    if (stat(to, &st) == 0) return error(req, "409 Conflict", "The destination already exists.");
    if (errno != ENOENT) return fs_error(req);
    if (rename(from, to)) return fs_error(req);
    event("Move", target, 0, 0, true, true);
    return httpd_resp_send(req, "Moved", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char path[PATH_CAP], full[FULL_CAP]; struct stat st;
    if (!parameter(req, "path", path) || !strcmp(path, "/")) return error(req, "400 Bad Request", "The SD card root cannot be deleted.");
    full_path(path, full);
    if (stat(full, &st)) return fs_error(req);
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(full);
        if (!dir) return fs_error(req);
        struct dirent *entry; bool empty = true;
        while ((entry = readdir(dir))) {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) { empty = false; break; }
        }
        closedir(dir);
        if (!empty) return error(req, "409 Conflict", "Folder is not empty. Delete its contents first.");
    }
    int result = S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
    if (result) {
        if (errno == ENOTEMPTY || errno == EEXIST) return error(req, "409 Conflict", "Folder is not empty. Delete its contents first.");
        return fs_error(req);
    }
    event("Delete", path, 0, 0, true, true);
    return httpd_resp_send(req, "Deleted", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t guarded(httpd_req_t *req)
{
    if (!file_server_acquire_storage()) return error(req, "503 Service Unavailable", "SD card busy. Wait for the current transfer or flash to finish.");
    workspace = malloc(sizeof(*workspace));
    if (!workspace) {
        file_server_release_storage();
        return error(req, "503 Service Unavailable", "Not enough memory for this file operation. Try again after other activity finishes.");
    }
    esp_err_t (*handler)(httpd_req_t *) = req->user_ctx;
    esp_err_t result = handler(req);
    free(workspace);
    workspace = NULL;
    unsigned remaining = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI("file_manager", "HTTP stack minimum free: %u bytes", remaining);
    if (remaining < 2048) ESP_LOGW("file_manager", "HTTP task stack headroom is low");
    file_server_release_storage();
    return result;
}

static esp_err_t favicon(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t file_manager_register(httpd_handle_t server, const char *mount_point, file_server_transfer_cb_t callback)
{
    snprintf(mount, sizeof(mount), "%s", mount_point);
    notify = callback;
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=root},
        {.uri="/favicon.ico", .method=HTTP_GET, .handler=favicon},
        {.uri="/api/list", .method=HTTP_GET, .handler=guarded, .user_ctx=list},
        {.uri="/api/download", .method=HTTP_GET, .handler=guarded, .user_ctx=download},
        {.uri="/api/upload", .method=HTTP_POST, .handler=guarded, .user_ctx=upload},
        {.uri="/api/mkdir", .method=HTTP_POST, .handler=guarded, .user_ctx=mkdir_handler},
        {.uri="/api/move", .method=HTTP_POST, .handler=guarded, .user_ctx=move_handler},
        {.uri="/api/delete", .method=HTTP_POST, .handler=guarded, .user_ctx=delete_handler},
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}
