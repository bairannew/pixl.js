/*
 * star_upload_hook.c  (v8 新增)
 *
 * 见 star_upload_hook.h. 实现尽量短小, 单文件, 无外部依赖.
 */
#include "star_upload_hook.h"
#include "nrf_log.h"

#include <string.h>

typedef struct {
    bool                   installed;
    const char            *match_prefix;   /* 调用方持有, 不复制 */
    star_upload_done_cb_t  cb;
    void                  *user_data;
} hook_state_t;

static hook_state_t s_hook = {0};

void star_upload_hook_set(const char *match_prefix,
                          star_upload_done_cb_t cb,
                          void *user_data) {
    s_hook.installed    = (cb != NULL);
    s_hook.match_prefix = match_prefix;
    s_hook.cb           = cb;
    s_hook.user_data    = user_data;
    NRF_LOG_DEBUG("star_upload_hook_set: installed=%d prefix=%s",
                  (int)s_hook.installed,
                  match_prefix ? match_prefix : "(any)");
}

void star_upload_hook_clear(void) {
    s_hook.installed    = false;
    s_hook.match_prefix = NULL;
    s_hook.cb           = NULL;
    s_hook.user_data    = NULL;
    NRF_LOG_DEBUG("star_upload_hook_clear");
}

bool star_upload_hook_installed(void) {
    return s_hook.installed;
}

void star_upload_hook_notify_close(const char *full_path, bool was_write) {
    if (!s_hook.installed || s_hook.cb == NULL) return;
    if (!was_write)                              return;   /* 读操作完全忽略 */
    if (full_path == NULL || full_path[0] == '\0') return;

    /* 前缀匹配 (NULL = 任意匹配) */
    if (s_hook.match_prefix != NULL) {
        size_t pn = strlen(s_hook.match_prefix);
        if (strncmp(full_path, s_hook.match_prefix, pn) != 0) {
            NRF_LOG_DEBUG("upload_hook: path=%s does not match prefix=%s",
                          full_path, s_hook.match_prefix);
            return;
        }
    }

    /* 复制成局部变量, 让回调里可以 clear 而不影响这次派发 */
    star_upload_done_cb_t cb = s_hook.cb;
    void *ud = s_hook.user_data;

    NRF_LOG_INFO("upload_hook: fire path=%s", full_path);
    cb(full_path, ud);
}
