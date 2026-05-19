/* v8.1 Star Badge navigator scenes - put CHANNEL_SELECT first so it is the default. */
ADD_SCENE(amiibo, channel_select,    CHANNEL_SELECT)
ADD_SCENE(amiibo, account_select,    ACCOUNT_SELECT)
ADD_SCENE(amiibo, account_input,     ACCOUNT_INPUT)
/* v8.2: 在 account 与 badge 之间插一层 category. 长按账号/分类
 * 会进入 action_menu, 让用户选 重命名/删除. */
ADD_SCENE(amiibo, category_select,   CATEGORY_SELECT)
ADD_SCENE(amiibo, category_input,    CATEGORY_INPUT)
ADD_SCENE(amiibo, action_menu,       ACTION_MENU)
ADD_SCENE(amiibo, badge_list,        BADGE_LIST)
/* v8.1 主流程: 点 "新建徽章" -> 立刻创建 new.bin 占位 + 挂 BLE 上传 hook ->
 * 直接进入 AMIIBO_DETAIL. BLE 写入到达时 hook 自动解析改名 + 刷新. */
ADD_SCENE(amiibo, badge_name_input,  BADGE_NAME_INPUT)
/* legacy: original generic browser (still compiled & reachable as fallback). */
ADD_SCENE(amiibo, file_browser, FILE_BROWSER)
ADD_SCENE(amiibo, file_browser_menu, FILE_BROWSER_MENU)
ADD_SCENE(amiibo, amiibo_detail, AMIIBO_DETAIL)
ADD_SCENE(amiibo, amiibo_detail_menu, AMIIBO_DETAIL_MENU)
