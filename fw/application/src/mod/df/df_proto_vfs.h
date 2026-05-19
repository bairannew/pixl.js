#ifndef DF_PROTO_VFS_H
#define DF_PROTO_VFS_H

#include "df_defines.h"

/** info proto command defines */
typedef enum {
    DF_PROTO_CMD_VFS_DRIVE_LIST = 0x10,
    DF_PROTO_CMD_VFS_DRIVE_FORMAT = 0x11,
    DF_PROTO_CMD_VFS_FILE_OPEN = 0x12,
    DF_PROTO_CMD_VFS_FILE_CLOSE = 0x13,
    DF_PROTO_CMD_VFS_FILE_READ = 0x14,
    DF_PROTO_CMD_VFS_FILE_WRITE = 0x15,
    DF_PROTO_CMD_VFS_DIR_READ = 0x16,
    DF_PROTO_CMD_VFS_DIR_CREATE = 0x17,
    DF_PROTO_CMD_VFS_REMOVE = 0x18,
    DF_PROTO_CMD_VFS_RENAME = 0x19,
    DF_PROTO_CMD_VFS_UPDATE_META = 0x1a
} df_proto_cmd_vfs_t;

extern const df_cmd_entry_t df_proto_handler_vfs_entries[];

/**
 * v8.2-fix11: 强制关闭 BLE 当前持有的上传文件句柄.
 *
 * 背景:
 *   df_proto 协议的 vfs file_open / file_write / file_close 在 BLE 客户端
 *   (网页 / 手机 app) 没有按预期关闭文件 (例如客户端 crash, 用户拔线断开),
 *   或处于打开和关闭之间的中间态时, 内部 file_chunk_state.opened 仍为 true,
 *   底层文件句柄保持占用.  LittleFS 在父目录/路径上有活的文件句柄时会拒绝
 *   rename / mkdir / remove, 导致设备端 UI 操作 (账号/分类重命名, 删除,
 *   孤儿徽章迁移) 在 "蓝牙连接模式下" 稳定失败.
 *
 *   v8.1-fix3 已经在 BLE 自己的 rename 路径里加了 "先关 active upload"
 *   的兜底, 但设备端 UI 走的是各自 scene 里直接调 p_drv->rename_dir(),
 *   完全绕过那段代码 — 因此 UI 端必须在 vfs 操作前调用本函数自行清理.
 *
 *   幂等: 没有打开的句柄时空操作.
 *
 * 安全保证:
 *   不会触发 star_upload_hook (因为这次关闭是 UI 主动剥夺, 数据完整性
 *   未知). 仅清理底层资源.
 */
void df_proto_vfs_close_active_upload(void);

/**
 * v9.0-fix2: 强制关闭 BLE 当前持有的上传"目录"句柄.
 *
 * 背景:
 *   df_proto 协议的 vfs_dir_read 是分块的 — BLE 客户端 (网页 / 手机 app)
 *   发起一次 dir_read 后, 设备端 dir_chunk_state 会保留 LFS 目录句柄,
 *   等客户端再发 "下一块" 数据请求时继续 read_dir.  在 "尚未读完" 这段
 *   时间里, 目录句柄是活的; 一旦该目录或其父目录被 rename / mkdir /
 *   remove, LittleFS 会因句柄占用拒绝, 表现为 "蓝牙连接下设备端 UI
 *   操作静默失败".
 *
 *   df_proto_vfs_close_active_upload() 只处理 file_chunk_state (单个
 *   文件句柄), 完全没有覆盖 dir_chunk_state.  v8.2-fix11 / v9.0-fix1 都
 *   只调了它 — 这是 "默认账号 / 默认分类下徽章列表为空" 与 "蓝牙模式下
 *   账号/分类重命名静默失败" 这两个高频报障的根因: BLE 客户端在用户
 *   操作的 ~100ms 间窗里仍在轮询 dir_read, dir 句柄留着, rename /
 *   migrate 全军覆没.
 *
 * 行为:
 *   - 若没有未关的目录句柄, 空操作 (幂等).
 *   - 否则调底层 close_dir 把句柄关掉, 同时把内部状态置为
 *     "已关 / 不再可恢复" — 客户端下一次发 DIR_READ 命令会被当成新会话
 *     从头 open_dir, 不会读到半截.
 *
 * 调用时机:
 *   设备端 UI 在 BLE 在线时进行 vfs rename / mkdir / remove 之前, 必须
 *   把 active_upload 和 active_dir 都调一遍 — 二者一起才能把 LFS 父
 *   目录上所有的 BLE 残留句柄清光.
 */
void df_proto_vfs_close_active_dir(void);

#endif