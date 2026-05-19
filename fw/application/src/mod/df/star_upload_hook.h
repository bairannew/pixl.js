/*
 * star_upload_hook.h  (v8 新增)
 *
 * "BLE/USB 上传完成" 一次性通知钩子.
 *
 * 背景:
 *   原版 v7 的"新建徽章"流程要求用户先用 BLE 把 .bin 推到 /star/_inbox/,
 *   再回到设备里手动选这个 .bin 当源文件. 体验非常糟 — 设备会显示
 *   "inbox 为空" 之类的提示, 完全不是 "点一下新建徽章就开始接收" 的直觉.
 *
 *   v8 改成 "原版加随机标签" 的肌肉记忆: 点一下"新建徽章" 就在当前
 *   <账号> 目录下创建一个 new.bin 占位, 同时进入 "等待 BLE 上传" 场景.
 *   BLE 那边的 df_proto_handler_vfs_file_close 完事后, 会调用本模块的
 *   star_upload_hook_notify_close("E:/star/.../xxx.bin"). 我们再把
 *   通知派发回当前等待中的 amiibo scene, 由它做自动识别 + 改名.
 *
 * 设计:
 *   - 全局只允许一个 "等待中的" hook (一次只能有一个 wait_upload scene).
 *   - 路径前缀匹配: hook 注册时传入 e.g. "E:/star/netease/default/",
 *     只有在该前缀下的文件 close 才触发回调.
 *   - 一次性: 触发后不会自动清, 要由调用方在回调里自己调
 *     star_upload_hook_clear() (或者直接 set 一个新的 hook 覆盖).
 *   - 线程模型: 整个 nRF52 firmware 都跑在 main loop / app_scheduler 上,
 *     不存在并发. 不加锁.
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) v8 - star_upload_hook ─
 */
#ifndef STAR_UPLOAD_HOOK_H
#define STAR_UPLOAD_HOOK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Hook 回调原型.
 *
 * @param full_path   刚关闭的完整路径 (含驱动器前缀, 例 "E:/star/.../x.bin").
 * @param user_data   注册时传入的用户指针 (一般是 app_amiibo_t*).
 */
typedef void (*star_upload_done_cb_t)(const char *full_path, void *user_data);

/**
 * 注册一个 "下次写完关闭" 的钩子.
 *
 * @param match_prefix  完整路径前缀, 例 "E:/star/netease/default/".
 *                      调用方持有该字符串的生命周期 (不复制, 不可指向栈).
 *                      传 NULL 等同 "任何路径都匹配".
 * @param cb            回调函数 (NULL 则相当于卸载).
 * @param user_data     回调时透传的用户指针.
 */
void star_upload_hook_set(const char *match_prefix,
                          star_upload_done_cb_t cb,
                          void *user_data);

/** 清除当前 hook. 离开 wait_upload 场景时必须调一次, 防止 dangling 回调. */
void star_upload_hook_clear(void);

/**
 * 由 BLE/USB 文件传输完成方调用 (df_proto_handler_vfs_file_close).
 * 若当前注册了 hook 且 full_path 以 match_prefix 开头 (或 prefix==NULL),
 * 则同步派发回调.
 *
 * 注意: 回调在本函数返回前同步执行; 调用方应当先把 BLE 响应帧发出去再调
 *      这个函数, 避免回调里 ~ms 级的 vfs / scene 切换阻塞 BLE 应答.
 *
 * @param full_path  刚关闭的完整路径 (含驱动器前缀).
 * @param was_write  这次打开是不是写操作 (false=只读, 我们不关心).
 */
void star_upload_hook_notify_close(const char *full_path, bool was_write);

/** 调试: 当前是否安装了 hook (主要用于断言). */
bool star_upload_hook_installed(void);

#ifdef __cplusplus
}
#endif

#endif /* STAR_UPLOAD_HOOK_H */
