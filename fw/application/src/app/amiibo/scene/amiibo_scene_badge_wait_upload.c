/*
 * amiibo_scene_badge_wait_upload.c   -- DEPRECATED in v8.1, intentionally empty.
 *
 * 历史:
 *   v8 在 badge_list 上点 "新建徽章" 后会进入这个独立场景, 它在 UI 上弹一个
 *   msg_box: "等待蓝牙上传\n完成后自动识别", 直到 BLE 写入到达, 然后再自动
 *   解析徽章 URL, 拿 sk 查库, 改名, 写入 vfs_meta, 最后再让用户去看徽章详情.
 *
 *   用户反馈: 这一步像是 "设备在叫我再做一次蓝牙上传", 体验上是多余的中间步骤.
 *   原话:
 *     "不要使用蓝牙上传空白BLE bin再进行感应接取BLE 啊
 *      而是自动创建一个空白的BLE bin，自动进入这个bin 然后自动接取BLE写入
 *      自动获取徽章BLE的URL 解密解析sk自动判断修改重命名名称
 *      而不是现在什么的叫用户蓝牙上传BLE
 *      这样太麻烦了 还要上传一次什么的"
 *
 * v8.1 处理:
 *   把原本属于本场景的全部逻辑 (占位文件创建, BLE hook 注册, NTAG 解析,
 *   sk 查表, 文件改名, 元数据写入, UI 刷新) 全部内联到 badge_list 里.
 *   - badge_list 点 "新建徽章" 时:
 *       1. ntag_store_new_rand 写一份 new.bin 占位 (若同名冲突就 new_2.bin ...)
 *       2. star_upload_hook_set 挂一个一次性的 BLE 上传回调
 *       3. 设好 current_folder / current_file / reload_amiibo_files
 *       4. 直接 next_scene -> AMIIBO_SCENE_AMIIBO_DETAIL
 *   - BLE 写入到达时, hook callback (badge_list_upload_hook_cb):
 *       1. 用 sky_badge_try_autofill 解析 NTAG dump -> URL -> sk -> entry
 *       2. 命中: 重命名为 "<中文名>.bin" + 写 vfs_meta.notes
 *       3. 未命中: 保留 new.bin, toast "未识别,长按可改名"
 *       4. 如果当前还停在 AMIIBO_DETAIL, 调 amiibo_scene_amiibo_detail_refresh
 *          让 UI 立刻显示重命名后的文件; 若已退回到 badge_list, 重新刷列表.
 *
 *   结果: 用户不再看到 "等待蓝牙上传" 的 msg_box, 流程在视觉上是
 *         "点新建 -> 看到一枚空徽章 -> (BLE 写到) -> 名字 / 详情自己变了".
 *
 * 文件保留原因:
 *   只为保留 git history (旧 519 行实现里有 UTF-8 截断 / vfs 路径处理等
 *   工程细节, 已迁到 amiibo_scene_badge_list.c). 本 .c 不会被新版 Makefile
 *   编译 (见 fw/application/Makefile, 已删去对应一行), 因此整个 TU 故意为空.
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) v8.1 ─
 */
