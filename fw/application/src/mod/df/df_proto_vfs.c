#include "df_proto_vfs.h"
#include "df_buffer.h"
#include "df_core.h"
#include "df_defines.h"
#include "nrf_log.h"
#include "vfs.h"
#include "star_upload_hook.h"   /* v8: BLE 上传完成回调钩子 */

#include <string.h>

static vfs_driver_t *get_driver_by_path(char *path) {
    if (path[0] == 'I') {
        return vfs_get_driver(VFS_DRIVE_INT);
    } else if (path[0] == 'E') {
        return vfs_get_driver(VFS_DRIVE_EXT);
    } else {
        return NULL;
    }
}

static char *get_file_path(char *path) { return path + VFS_DRIVE_LABEL_LEN; }

static uint8_t get_meta_size(uint8_t *meta) {
    uint8_t meta_size = meta[0];
    return meta_size == 0 || meta_size == 0xff ? 0 : meta_size;
}

static bool validate_path(char *path) {
    if (path[0] != 'I' && path[0] != 'E') {
        return false;
    }
    if (path[1] != ':' || path[2] != '/') {
        return false;
    }

    return true;
}

void df_proto_handler_vfs_drive_list(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out = {0};
        vfs_stat_t stat = {0};

        NEW_BUFFER_ZERO(buff, out.data, sizeof(out.data));

        uint8_t drv_cnt = vfs_drive_enabled(VFS_DRIVE_INT) + vfs_drive_enabled(VFS_DRIVE_EXT);
        buff_put_u8(&buff, drv_cnt); // drive count

        if (vfs_drive_enabled(VFS_DRIVE_INT)) {
            vfs_driver_t *p_driver = vfs_get_driver(VFS_DRIVE_EXT);

            if (p_driver->stat(&stat) == VFS_OK && !stat.avaliable) {
                p_driver->mount();
            }

            if (p_driver->stat(&stat) == VFS_OK && !stat.avaliable) {
                buff_put_u8(&buff, stat.avaliable); // drive status code
                buff_put_char(&buff, 'I');          // drive label
                buff_put_string(&buff, "Internal Flash");
                buff_put_u32(&buff, stat.total_bytes); // total space
                buff_put_u32(&buff, stat.free_bytes);  // free space
            } else {
                buff_put_u8(&buff, 1);     // drive status code
                buff_put_char(&buff, 'I'); // drive label
                buff_put_string(&buff, "Internal Flash");
                buff_put_u32(&buff, 0); // total space
                buff_put_u32(&buff, 0); // free space
            }
        }

        if (vfs_drive_enabled(VFS_DRIVE_EXT)) {
            vfs_driver_t *p_driver = vfs_get_driver(VFS_DRIVE_EXT);

            if (p_driver->stat(&stat) == VFS_OK && !stat.avaliable) {
                p_driver->mount();
            }

            if (p_driver->stat(&stat) == VFS_OK) {
                buff_put_u8(&buff, stat.avaliable ? 0 : 1); // drive status code
                buff_put_char(&buff, 'E');                  // drive label
                buff_put_string(&buff, "External Flash");
                buff_put_u32(&buff, stat.total_bytes); // total space
                buff_put_u32(&buff, stat.free_bytes);  // free space
            } else {
                buff_put_u8(&buff, 1);     // drive status code
                buff_put_char(&buff, 'E'); // drive label
                buff_put_string(&buff, "External Flash");
                buff_put_u32(&buff, 0); // total space
                buff_put_u32(&buff, 0); // used space
            }
        }

        OUT_FRAME_WITH_DATA_0(out, evt->df->cmd, DF_STATUS_OK, buff_get_size(&buff));

        df_core_send_frame(&out);
    }
}

void df_proto_handler_vfs_drive_format(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out;

        NEW_BUFFER(buff, evt->df->data, evt->df->length);
        char drv_label = (char)buff_get_u8(&buff);
        vfs_driver_t *p_driver = get_driver_by_path(&drv_label);
        if (p_driver == NULL) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        if (p_driver->format() == VFS_OK) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_OK);
        } else {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
        }
        df_core_send_frame(&out);
    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
    }
}

typedef struct {
    vfs_dir_t dir;
    vfs_obj_t obj;
    bool obj_consumed;
    bool dir_closed;
    uint16_t chunk;
    vfs_driver_t *driver;
    /* v9.0-fix2: 新增 "句柄是否仍在 LFS 那里活着" 显式标志.
     *
     * 历史问题: 旧版只有 dir_closed (默认 false, 表示"还没读完"),
     * 但它在 BSS 静态零初始化时也是 false — 也就是说 "从未 open 过"
     * 也是 false, 不能用 !dir_closed 判 "是否需要 close".
     *
     * opened 由 open_dir 成功后置 true, close_dir (任何路径) 后置 false,
     * 初始 false. 这样 close_active_dir() 才能安全幂等地判断要不要关. */
    bool opened;
} dir_chunk_state_t;

/* v9.0-fix2: 提到文件级, 让 df_proto_vfs_close_active_dir() 也能访问.
 * 原来定义在 df_proto_handler_vfs_dir_read 函数体内, 跨函数无法清理. */
static dir_chunk_state_t dir_chunk_state = { .opened = false, .dir_closed = true, .driver = NULL };

static void dir_read_send_chunk(dir_chunk_state_t *chunk_state, df_frame_t *out) {

    NEW_BUFFER_ZERO(buff, out->data, sizeof(out->data));

    if (chunk_state->dir_closed) {
        return;
    }

    if (!chunk_state->obj_consumed) {
        uint8_t meta_size = get_meta_size(chunk_state->obj.meta);
        buff_put_string(&buff, chunk_state->obj.name);
        buff_put_u32(&buff, chunk_state->obj.size);
        buff_put_u8(&buff, chunk_state->obj.type);
        buff_put_u8(&buff, meta_size);
        if (meta_size > 0) {
            buff_put_byte_array(&buff, chunk_state->obj.meta + 1, meta_size);
        }
        chunk_state->obj_consumed = true;
    }

    while ((chunk_state->driver->read_dir(&chunk_state->dir, &chunk_state->obj)) == VFS_OK) {
        uint8_t meta_size = get_meta_size(chunk_state->obj.meta);
        uint8_t size_required = strlen(chunk_state->obj.name) + meta_size + 8;
        if (buffer_get_available_cap(&buff) >= size_required) {
            buff_put_string(&buff, chunk_state->obj.name);
            buff_put_u32(&buff, chunk_state->obj.size);
            buff_put_u8(&buff, chunk_state->obj.type);
            buff_put_u8(&buff, meta_size);
            if (meta_size > 0) {
                buff_put_byte_array(&buff, chunk_state->obj.meta + 1, meta_size);
            }
            chunk_state->obj_consumed = true;
        } else {
            chunk_state->obj_consumed = false;
            break;
        }
    }

    out->cmd = DF_PROTO_CMD_VFS_DIR_READ;
    if (chunk_state->obj_consumed) {
        out->chunk = chunk_state->chunk;
        chunk_state->dir_closed = true;
        chunk_state->driver->close_dir(&chunk_state->dir);
        /* v9.0-fix2: 同步清掉 opened, 让外部 close_active_dir() 不再
         * 误以为还有活的句柄. 老路径 (本次自然读完) 走到这里. */
        chunk_state->opened = false;
    } else {
        out->chunk = 0x8000 | chunk_state->chunk;
    }
    out->status = DF_STATUS_OK;
    out->length = buff_get_size(&buff);

    chunk_state->chunk++;
    df_core_send_frame(out);
}

void df_proto_handler_vfs_dir_read(df_event_t *evt) {

    /* v9.0-fix2: 不再使用本函数内的 static 变量, 改用文件级 dir_chunk_state.
     * 这样 df_proto_vfs_close_active_dir() (UI scene 在 rename / migrate 前
     * 调) 才能跨函数清掉同一个句柄. 别名只是为了 patch 改动最小. */
    dir_chunk_state_t *p_chunk_state = &dir_chunk_state;
    df_frame_t out;

    if (evt->type == DF_EVENT_DATA_RECEVIED) {

        NEW_BUFFER(buff, evt->df->data, evt->df->length);

        char path[VFS_MAX_FULL_PATH_LEN];
        memset(path, 0, sizeof(path));
        buff_get_string(&buff, path, sizeof(path));

        /* v9.0-fix2: 上一次 dir_read 没读完就被打断 (客户端发了新的请求 / 切目录),
         * 老句柄必须先关掉, 否则会泄漏直到下次自然 close_dir, 期间任何 UI
         * 端的 rename 都会因 LFS 句柄占用失败. */
        if (p_chunk_state->opened && p_chunk_state->driver != NULL) {
            p_chunk_state->driver->close_dir(&p_chunk_state->dir);
            p_chunk_state->opened = false;
            p_chunk_state->dir_closed = true;
        }

        p_chunk_state->driver = get_driver_by_path(path);
        if (p_chunk_state->driver == NULL) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        int32_t err = p_chunk_state->driver->open_dir(get_file_path(path), &p_chunk_state->dir);
        if (err) {
            // TODO mapping error
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        p_chunk_state->chunk = 0;
        p_chunk_state->dir_closed = false;
        p_chunk_state->obj_consumed = true;
        /* v9.0-fix2: 句柄进入 "活" 态. close_active_dir() 看到这个就会关. */
        p_chunk_state->opened = true;

        dir_read_send_chunk(p_chunk_state, &out);
    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
        dir_read_send_chunk(p_chunk_state, &out);
    } else if (evt->type == DF_EVENT_LINK_DISCONNECTED) {
        /* v9.0-fix2: BLE 断链时把残余的 dir 句柄关干净. 之前是空分支,
         * 客户端断线后句柄会一直挂着, 直到下次 open_dir 才被换掉 — 在
         * 那之前任何 rename / migrate 都会卡 LFS. */
        if (p_chunk_state->opened && p_chunk_state->driver != NULL) {
            p_chunk_state->driver->close_dir(&p_chunk_state->dir);
            p_chunk_state->opened = false;
            p_chunk_state->dir_closed = true;
            NRF_LOG_INFO("dir_read: link disconnected, force-closed active dir handle");
        }
    }
}

void df_proto_handler_vfs_dir_create(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out;

        NEW_BUFFER(buff, evt->df->data, evt->df->length);
        char path[VFS_MAX_FULL_PATH_LEN];
        memset(path, 0, sizeof(path));
        buff_get_string(&buff, path, sizeof(path));

        vfs_driver_t *p_driver = get_driver_by_path(path);
        if (p_driver == NULL) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        if (p_driver->create_dir(get_file_path(path)) == VFS_OK) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_OK);
        } else {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
        }
        df_core_send_frame(&out);
    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
    }
}

void df_proto_handler_vfs_remove(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out;

        NEW_BUFFER(buff, evt->df->data, evt->df->length);
        char path[VFS_MAX_FULL_PATH_LEN];
        memset(path, 0, sizeof(path));
        buff_get_string(&buff, path, sizeof(path));

        vfs_driver_t *p_driver = get_driver_by_path(path);
        if (p_driver == NULL) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        vfs_obj_t obj;
        if (p_driver->stat_file(get_file_path(path), &obj) != VFS_OK) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        int32_t err = 0;
        if (obj.type == VFS_TYPE_DIR) {
            err = p_driver->remove_dir(get_file_path(path));
        } else {
            err = p_driver->remove_file(get_file_path(path));
        }

        if (err != VFS_OK) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }
        OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_OK);
        df_core_send_frame(&out);

    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
    }
}

typedef struct {
    bool opened;
    vfs_file_t vfs_fd;
    vfs_driver_t *vfs_driver;
    int32_t err_code;
    uint16_t chunk;
    /* v8: 记住打开时的完整路径 + 是否是写模式, 用来在 close 后派发 upload hook */
    char     last_path[VFS_MAX_FULL_PATH_LEN];
    bool     last_was_write;
} file_chunk_state_t;

static file_chunk_state_t file_chunk_state = {0};

void df_proto_handler_vfs_file_open(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out;
        uint32_t flags;
        int32_t err_code;

        if (file_chunk_state.opened) {
            file_chunk_state.vfs_driver->close_file(&file_chunk_state.vfs_fd);
            file_chunk_state.opened = false;
            /* v8: 强制关闭的旧句柄, 不触发 upload hook (BLE 没正常 close, 数据
             * 完整性存疑). 仅清掉 last_path/last_was_write 防 hook 误触发. */
            file_chunk_state.last_path[0] = '\0';
            file_chunk_state.last_was_write = false;
        }

        NEW_BUFFER_READ(buff, evt->df->data, evt->df->length);
        char path[VFS_MAX_FULL_PATH_LEN];
        memset(path, 0, sizeof(path));
        buff_get_string(&buff, path, sizeof(path));
        flags = buff_get_u32(&buff);

        vfs_driver_t *p_driver = get_driver_by_path(path);
        if (p_driver == NULL) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        err_code = p_driver->open_file(get_file_path(path), &file_chunk_state.vfs_fd, flags);
        if (err_code != VFS_OK) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        file_chunk_state.opened = true;
        file_chunk_state.vfs_driver = p_driver;
        file_chunk_state.err_code = VFS_OK;

        /* v8: 记录路径 + 是否带写权限, 用于 close 后通知 upload hook.
         * 注意 path[] 已经在前面 memset+memcpy, 是完整 "X:/..." 形态. */
        strncpy(file_chunk_state.last_path, path, sizeof(file_chunk_state.last_path) - 1);
        file_chunk_state.last_path[sizeof(file_chunk_state.last_path) - 1] = '\0';
        file_chunk_state.last_was_write =
            (flags & (VFS_MODE_TRUNC | VFS_MODE_CREATE |
                      VFS_MODE_APPEND | VFS_MODE_WRITEONLY)) != 0;

        NEW_BUFFER_ZERO(out_buff, out.data, sizeof(out.data));
        buff_put_u8(&out_buff, 0); // always 0

        OUT_FRAME_WITH_DATA_0(out, evt->df->cmd, DF_STATUS_OK, buff_get_size(&out_buff));
        df_core_send_frame(&out);

        return;
    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
    }
}

void df_proto_handler_vfs_file_close(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out;
        int32_t err_code;

        NEW_BUFFER_READ(buff, evt->df->data, evt->df->length);

        uint8_t file_id = buff_get_u8(&buff); // ignore
        if (!file_chunk_state.opened) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        err_code = file_chunk_state.vfs_driver->close_file(&file_chunk_state.vfs_fd);
        if (err_code != VFS_OK) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        /* v8 关键: 上传成功. 把 BLE 应答先发出去, 再同步派发 upload hook.
         * 顺序很重要 — 否则 hook 里 ~ms 级的 vfs 操作会拖死 BLE 应答, 让前端
         * 误以为关闭超时. */
        file_chunk_state.opened = false;
        OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_OK);
        df_core_send_frame(&out);

        /* close 成功 && 这次打开带写权限 => 这是一次 "上传写入完成" */
        if (file_chunk_state.last_was_write &&
            file_chunk_state.last_path[0] != '\0') {
            char path_copy[VFS_MAX_FULL_PATH_LEN];
            strncpy(path_copy, file_chunk_state.last_path, sizeof(path_copy) - 1);
            path_copy[sizeof(path_copy) - 1] = '\0';
            /* 清掉状态, 防止 hook 里再次调用 close 走回环 */
            file_chunk_state.last_path[0] = '\0';
            file_chunk_state.last_was_write = false;
            star_upload_hook_notify_close(path_copy, true);
        } else {
            file_chunk_state.last_path[0] = '\0';
            file_chunk_state.last_was_write = false;
        }
    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
    }
}

void df_proto_handler_vfs_file_write(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out;

        NEW_BUFFER_READ(buff, evt->df->data, evt->df->length);

        uint8_t file_id = buff_get_u8(&buff); // ignore

        bool chunk_eof = !(evt->df->chunk & 0x8000);

        if (file_chunk_state.opened && file_chunk_state.err_code == VFS_OK) {
            void *data_buff = buff_get_data_ptr_pos(&buff);
            size_t data_size = buff_get_remain_size(&buff);

            int32_t bytes_written =
                file_chunk_state.vfs_driver->write_file(&file_chunk_state.vfs_fd, data_buff, data_size);
            if (bytes_written < 0) {
                file_chunk_state.err_code = bytes_written;
            } else if ((size_t)bytes_written != data_size) {
                /* v7 fix: 短写入也必须报错, 否则前端会把不完整的 .bin 当作上传成功,
                 * 之后 "添加徽章" 会拿到一个内容不全的源文件而稳定失败. */
                NRF_LOG_ERROR("vfs file write short write: want=%u got=%d",
                              (unsigned)data_size, bytes_written);
                file_chunk_state.err_code = VFS_ERR_FAIL;
            }
        }

        if (chunk_eof) {
            if (file_chunk_state.opened && file_chunk_state.err_code == VFS_OK) {
                OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_OK);
            } else {
                OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            }

            df_core_send_frame(&out);
        }
    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
    }
}

void file_read_send_chunk(file_chunk_state_t *chunk_state, df_frame_t *out) {
    if (!chunk_state->opened || chunk_state->err_code != VFS_OK) {
        return;
    }

    void *data_buff = out->data;
    size_t data_size = sizeof(out->data);
    bool chunk_eof = false;

    int32_t bytes_read = chunk_state->vfs_driver->read_file(&chunk_state->vfs_fd, data_buff, data_size);
    if (bytes_read < 0) {
        chunk_state->err_code = bytes_read;
        out->status = bytes_read;
        chunk_eof = true;
        out->length = 0;
    } else if (bytes_read < data_size) {
        chunk_state->err_code = VFS_ERR_EOF;
        out->status = DF_STATUS_OK;
        chunk_eof = true;
        out->length = bytes_read;
    } else {
        chunk_state->err_code = VFS_OK;
        out->status = DF_STATUS_OK;
        out->length = data_size;
    }

    out->cmd = DF_PROTO_CMD_VFS_FILE_READ;
    if (chunk_eof) {
        out->chunk = chunk_state->chunk;
    } else {
        out->chunk = chunk_state->chunk | 0x8000;
    }

    chunk_state->chunk++;
    df_core_send_frame(out);
}

void df_proto_handler_vfs_rename(df_event_t *evt) {
    if (evt->type == DF_EVENT_DATA_RECEVIED) {
        df_frame_t out;

        NEW_BUFFER_READ(buff, evt->df->data, evt->df->length);
        char old_path[VFS_MAX_FULL_PATH_LEN] = {0};
        char new_path[VFS_MAX_FULL_PATH_LEN] = {0};

        buff_get_string(&buff, old_path, VFS_MAX_FULL_PATH_LEN);
        buff_get_string(&buff, new_path, VFS_MAX_FULL_PATH_LEN);

        if (!validate_path(old_path) || !validate_path(new_path)) {
            NRF_LOG_INFO("rename: path error old=%s new=%s", old_path, new_path);
            OUT_FRAME_NO_DATA(out, DF_PROTO_CMD_VFS_RENAME, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        if (get_driver_by_path(old_path) != get_driver_by_path(new_path)) {
            NRF_LOG_INFO("rename: different drive");
            OUT_FRAME_NO_DATA(out, DF_PROTO_CMD_VFS_RENAME, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        vfs_driver_t *p_driver = get_driver_by_path(old_path);
        if (p_driver == NULL) {
            NRF_LOG_INFO("rename: vfs driver is not found");
            OUT_FRAME_NO_DATA(out, DF_PROTO_CMD_VFS_RENAME, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        /* v8.1-fix3: 如果当前有文件正在被 BLE 传输打开, 先关闭它.
         * 某些 VFS 驱动 (特别是 LittleFS) 在同一目录下有打开的文件句柄时
         * 拒绝 rename, 导致 "蓝牙传输模式下无法重命名". */
        if (file_chunk_state.opened) {
            p_driver->close_file(&file_chunk_state.vfs_fd);
            file_chunk_state.opened = false;
            file_chunk_state.last_path[0] = '\0';
            file_chunk_state.last_was_write = false;
            NRF_LOG_INFO("rename: force-closed open file handle before rename");
        }

        /* v9.0-fix2: 同理 — BLE dir_read 分块协议会在客户端两次发包之间
         * 保留 dir 句柄, 如果客户端在发 rename 之前刚好处于 "读到一半"
         * 的状态, LFS 会因父目录有活句柄拒绝 rename. 这里把 dir 句柄
         * 一并关掉, 跟 file 句柄一起清光. */
        if (dir_chunk_state.opened && dir_chunk_state.driver != NULL) {
            dir_chunk_state.driver->close_dir(&dir_chunk_state.dir);
            dir_chunk_state.opened = false;
            dir_chunk_state.dir_closed = true;
            NRF_LOG_INFO("rename: force-closed open dir handle before rename");
        }

        vfs_obj_t obj;
        int32_t err;

        err = p_driver->stat_file(get_file_path(old_path), &obj);

        if (err == VFS_OK && obj.type == VFS_TYPE_DIR) {
            err = p_driver->rename_dir(get_file_path(old_path), get_file_path(new_path));
        } else {
            /* v8.1-fix3: 即使 stat_file 失败也尝试 rename_file —
             * stat 可能因 VFS 缓存 / 路径编码 / 并发写入等原因暂时失败,
             * 但实际 rename 仍然可以成功. */
            err = p_driver->rename_file(get_file_path(old_path), get_file_path(new_path));
            if (err != VFS_OK) {
                /* 文件 rename 也失败, 最后尝试当目录 rename */
                int32_t dir_err = p_driver->rename_dir(get_file_path(old_path), get_file_path(new_path));
                if (dir_err == VFS_OK) {
                    err = VFS_OK;
                }
            }
        }

        if (err != VFS_OK) {
            NRF_LOG_INFO("rename error: %d (old=%s new=%s)", err,
                         nrf_log_push(old_path), nrf_log_push(new_path));
            OUT_FRAME_NO_DATA(out, DF_PROTO_CMD_VFS_RENAME, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        OUT_FRAME_NO_DATA(out, DF_PROTO_CMD_VFS_RENAME, DF_STATUS_OK);
        df_core_send_frame(&out);
    }
}

void df_proto_handler_vfs_file_read(df_event_t *evt) {
    df_frame_t out;
    if (evt->type == DF_EVENT_DATA_RECEVIED) {

        NEW_BUFFER_READ(buff, evt->df->data, evt->df->length);

        uint8_t file_id = buff_get_u8(&buff); // ignore

        file_chunk_state.chunk = 0;
        file_chunk_state.err_code = VFS_OK;

        if (!file_chunk_state.opened) {
            OUT_FRAME_NO_DATA(out, DF_PROTO_CMD_VFS_FILE_READ, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        file_read_send_chunk(&file_chunk_state, &out);

    } else if (evt->type == DF_EVENT_DATA_TRANSMIT_READY) {
        file_read_send_chunk(&file_chunk_state, &out);
    }
}

void df_proto_handler_vfs_update_meta(df_event_t *evt) {
    df_frame_t out;
    if (evt->type == DF_EVENT_DATA_RECEVIED) {

        NEW_BUFFER_READ(buff, evt->df->data, evt->df->length);

        char path[VFS_MAX_FULL_PATH_LEN];
        memset(path, 0, sizeof(path));
        buff_get_string(&buff, path, sizeof(path));

        uint8_t meta[VFS_MAX_META_LEN];
        memset(meta, 0, sizeof(meta));
        uint8_t meta_size = buff_get_u8(&buff);
        if (meta_size > 0) {
            buff_get_byte_array(&buff, meta + 1, meta_size);
        }
        meta[0] = meta_size;
        vfs_driver_t *p_driver = get_driver_by_path(path);
        if (p_driver == NULL) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        if (p_driver->update_file_meta(get_file_path(path), meta, meta_size + 1) != VFS_OK) {
            OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_ERR);
            df_core_send_frame(&out);
            return;
        }

        OUT_FRAME_NO_DATA(out, evt->df->cmd, DF_STATUS_OK);
        df_core_send_frame(&out);
    }
}

const df_cmd_entry_t df_proto_handler_vfs_entries[] = {
    {DF_PROTO_CMD_VFS_DRIVE_LIST, df_proto_handler_vfs_drive_list},
    {DF_PROTO_CMD_VFS_DRIVE_FORMAT, df_proto_handler_vfs_drive_format},
    {DF_PROTO_CMD_VFS_DIR_READ, df_proto_handler_vfs_dir_read},
    {DF_PROTO_CMD_VFS_DIR_CREATE, df_proto_handler_vfs_dir_create},
    {DF_PROTO_CMD_VFS_REMOVE, df_proto_handler_vfs_remove},
    {DF_PROTO_CMD_VFS_RENAME, df_proto_handler_vfs_rename},
    {DF_PROTO_CMD_VFS_FILE_OPEN, df_proto_handler_vfs_file_open},
    {DF_PROTO_CMD_VFS_FILE_CLOSE, df_proto_handler_vfs_file_close},
    {DF_PROTO_CMD_VFS_FILE_WRITE, df_proto_handler_vfs_file_write},
    {DF_PROTO_CMD_VFS_FILE_READ, df_proto_handler_vfs_file_read},
    {DF_PROTO_CMD_VFS_UPDATE_META, df_proto_handler_vfs_update_meta},
    {0, NULL}};


/* ============================================================ */
/*  v8.2-fix11: 公共 API — 强制关闭 BLE 当前持有的上传文件句柄.    */
/*                                                                */
/*  设备端 UI scene (account_input / category_input / badge_list  */
/*  migrate / action_menu delete 等) 在 BLE 连接状态下进行 vfs    */
/*  操作前必须调用本函数, 防止 LittleFS 因句柄占用拒绝 rename /   */
/*  mkdir / remove.                                              */
/*                                                                */
/*  设计:                                                         */
/*    - 幂等: 没有打开的句柄时空操作.                              */
/*    - 不触发 star_upload_hook: 这次关闭是 UI 主动剥夺, BLE 没    */
/*      正常 close, 数据完整性未知.                                */
/*    - 同步操作: 在调用方返回前已经清理完毕, 可直接接 rename.     */
/* ============================================================ */
void df_proto_vfs_close_active_upload(void) {
    if (!file_chunk_state.opened) {
        /* 已经关上 - 不做无效 IO. last_path 也清一下 (防止旧的 path
         * 残留在 close 后被 hook 误派发, 虽然 opened=false 时 hook
         * 已经不会被 file_close 主动触发, 但稳一点). */
        file_chunk_state.last_path[0] = '\0';
        file_chunk_state.last_was_write = false;
        return;
    }

    if (file_chunk_state.vfs_driver != NULL) {
        file_chunk_state.vfs_driver->close_file(&file_chunk_state.vfs_fd);
    }
    file_chunk_state.opened = false;
    /* 强制关闭, 不调 star_upload_hook_notify_close — 数据完整性未知,
     * 让用户在 UI 上看到的是: "rename/delete 成功, 没有自动改名". */
    file_chunk_state.last_path[0] = '\0';
    file_chunk_state.last_was_write = false;
    NRF_LOG_INFO("df_proto_vfs_close_active_upload: forced close (UI requested vfs op)");
}


/* ============================================================ */
/*  v9.0-fix2: 公共 API — 强制关闭 BLE 当前持有的上传目录句柄.    */
/*                                                                */
/*  设备端 UI scene 在 BLE 在线时进行 rename / mkdir / remove 之前 */
/*  必须把 active_upload 和 active_dir 都关一遍 — 二者一起才能把  */
/*  LFS 父目录上所有的 BLE 残留句柄清光.                          */
/*                                                                */
/*  这是修 v8.2-fix11 / v9.0-fix1 留下的一个未覆盖路径: dir_read   */
/*  是分块协议, 客户端在两次发包之间设备会保留 dir 句柄, UI 在     */
/*  那个 ~100ms 间窗里的 rename / migrate 全部会被 LFS 拒.        */
/*                                                                */
/*  幂等 + 安全:                                                  */
/*    - 没有未关的目录句柄, 空操作.                                */
/*    - close 之后状态被打回 "已结束 + 已关", 客户端下次发        */
/*      DIR_READ 会被当成新会话从头 open_dir, 不会读到半截.        */
/* ============================================================ */
void df_proto_vfs_close_active_dir(void) {
    if (!dir_chunk_state.opened) {
        /* 没活的句柄, 啥也不做. */
        return;
    }
    if (dir_chunk_state.driver != NULL) {
        dir_chunk_state.driver->close_dir(&dir_chunk_state.dir);
    }
    dir_chunk_state.opened = false;
    dir_chunk_state.dir_closed = true;
    /* 注意: chunk / obj 不清, 是怕极端情况下还在 TX 队列里的旧应答帧
     * 再次进入 dir_read_send_chunk 时拿到陈数据. 那条路径会先看
     * dir_closed=true 立刻 return, 不会真访问 dir/obj. */
    NRF_LOG_INFO("df_proto_vfs_close_active_dir: forced close (UI requested vfs op)");
}