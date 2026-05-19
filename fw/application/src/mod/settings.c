#include "settings.h"
#include "nrf_error.h"
#include "nrf_log.h"
#include "vfs.h"
#include "vfs_meta.h"

#include "tag_helper.h"
#include "ble_amiibolink.h"

#define SETTINGS_FILE_NAME "/settings.bin"

#ifdef OLED_SCREEN
// Though OLED doesn't necessarily imply rechargeable battery, it's usually the case.
#define DEFAULT_BAT_MODE 1
#else // !OLED_SCREEN
#define DEFAULT_BAT_MODE 0
#endif // OLED_SCREEN

const settings_data_t def_settings_data = {.backlight = 0,
                                           .auto_gen_amiibo = 0,
                                           .auto_gen_amiibolink = 0,
                                           .sleep_timeout_sec = 30,
                                           .skip_driver_select = 0,
                                           .bat_mode = DEFAULT_BAT_MODE,
                                           .amiibo_link_ver = BLE_AMIIBOLINK_VER_V1,
                                           .language = LANGUAGE_ZH_HANS, // [光遇定制] 默认简体中文
                                           .hibernate_enabled = false,
                                           .show_mem_usage = false,
                                           .lcd_backlight = 0,
                                           .oled_contrast = 80,
                                           /* v8.1-fix2: 系统动画强制默认开启 (用户要求).
                                            * 设置菜单里"动画效果"那一项也一并删掉. validate_settings()
                                            * 会把已经存盘的 anim_enabled=0 也强制改回 1, 兼容老用户. */
                                           .anim_enabled = true,
                                           .amiidb_data_slot_num = 20,
                                           .qrcode_enabled = true,
                                           .chameleon_default_slot_index = INVALID_SLOT_INDEX,
                                            .app_enable_bits = 0xFFFF,
                                            .amiidb_sort_column = 0,
                                            .chameleon_slot_num = 8,
                                            .amiibolink_mode = 0, // 0 = not set, use default (manual)
                                        };

settings_data_t m_settings_data = {0};

#define BOOL_VALIDATE(expr, default_val)                                                                               \
    if ((expr) != 0 && (expr) != 1) {                                                                                  \
        (expr) = (default_val);                                                                                        \
    }
#define INT8_VALIDATE(expr, min, max, default_val)                                                                     \
    if ((expr) < (min) || (expr) > (max)) {                                                                            \
        (expr) = (default_val);                                                                                        \
    }

static void validate_settings() {
    if (m_settings_data.sleep_timeout_sec > 180) {
        m_settings_data.sleep_timeout_sec = 30;
    }

    if (m_settings_data.amiibo_link_ver != BLE_AMIIBOLINK_VER_V1 &&
        m_settings_data.amiibo_link_ver != BLE_AMIIBOLINK_VER_V2 && m_settings_data.amiibo_link_ver != BLE_AMILOOP) {
        m_settings_data.amiibo_link_ver = BLE_AMIIBOLINK_VER_V1;
    }

    BOOL_VALIDATE(m_settings_data.skip_driver_select, 0);
    BOOL_VALIDATE(m_settings_data.show_mem_usage, 0);
    BOOL_VALIDATE(m_settings_data.hibernate_enabled, 0);
    BOOL_VALIDATE(m_settings_data.bat_mode, 0);
    BOOL_VALIDATE(m_settings_data.skip_driver_select, 0);
    BOOL_VALIDATE(m_settings_data.auto_gen_amiibo, 0);
    BOOL_VALIDATE(m_settings_data.auto_gen_amiibolink, 0);
    BOOL_VALIDATE(m_settings_data.backlight, 0);
    INT8_VALIDATE(m_settings_data.lcd_backlight, 0, 100, 0);
    INT8_VALIDATE(m_settings_data.oled_contrast, 0, 100, 80);
    /* v8.1-fix2: 不再让 anim_enabled 接受 0. 设置里没法关, 老存盘里若有
     * anim_enabled=0 (上一版本默认值就是 0), 在这里强制改回 1. */
    m_settings_data.anim_enabled = true;
    BOOL_VALIDATE(m_settings_data.qrcode_enabled, 0);
    /* v8.1-fix2: 老 settings.bin 里 language 可能存了 0..14 (15 个旧枚举值),
     * 新枚举只有 0 (ZH_HANS) 和 1 (ZH_TW).
     *   旧值 0 (ZH_HANS) → 仍是简体, 留作 0 ✓
     *   旧值 2 (ZH_TW)   → 旧 ZH_TW, 现在被新 enum 占用为别的语言, 改成 1 (新 ZH_TW)
     *   其它 1..14       → 一律回简体
     */
    if (m_settings_data.language == 2) {
        /* 旧 LANGUAGE_ZH_TW 枚举值是 2, 新枚举值改成了 1, 这里平移. */
        m_settings_data.language = LANGUAGE_ZH_TW;
    } else if (m_settings_data.language >= LANGUAGE_COUNT) {
        /* 其它所有"过去存在过, 现在删掉了"的语言值, 一律回简体. */
        m_settings_data.language = LANGUAGE_ZH_HANS;
    }
    INT8_VALIDATE(m_settings_data.language, 0, LANGUAGE_COUNT - 1, LANGUAGE_ZH_HANS);
    INT8_VALIDATE(m_settings_data.amiidb_data_slot_num, 1, 100, 20);
    INT8_VALIDATE(m_settings_data.chameleon_slot_num, 8, 50, 8);
    INT8_VALIDATE(m_settings_data.chameleon_default_slot_index, 0, m_settings_data.chameleon_slot_num, INVALID_SLOT_INDEX);
    
    // Validate amiibolink_mode: 0 = not set, 1-4 are valid modes
    if (m_settings_data.amiibolink_mode != 0 && 
        m_settings_data.amiibolink_mode != BLE_AMIIBOLINK_MODE_RANDOM &&
        m_settings_data.amiibolink_mode != BLE_AMIIBOLINK_MODE_CYCLE &&
        m_settings_data.amiibolink_mode != BLE_AMIIBOLINK_MODE_NTAG &&
        m_settings_data.amiibolink_mode != BLE_AMIIBOLINK_MODE_RANDOM_AUTO_GEN) {
        m_settings_data.amiibolink_mode = 0; // Reset to "not set" if invalid
    }
}

int32_t settings_init() {
    memcpy(&m_settings_data, &def_settings_data, sizeof(settings_data_t));
    vfs_driver_t *p_driver = vfs_get_default_driver();
    if (p_driver == NULL) {
        return NRF_ERROR_NOT_SUPPORTED;
    }
    int32_t err = 0;
    if (!p_driver->mounted()) {
        err = p_driver->mount();
    }

    if (!p_driver->mounted()) {
        return NRF_ERROR_INVALID_STATE;
    }

    err = p_driver->read_file_data(SETTINGS_FILE_NAME, &m_settings_data, sizeof(settings_data_t));
    if (err < 0) {
        return NRF_ERROR_INVALID_STATE;
    }

    validate_settings();

    NRF_LOG_INFO("settings loaded!");
    return NRF_SUCCESS;
}

int32_t settings_save() {
    vfs_driver_t *p_driver = vfs_get_default_driver();
    int32_t err;

    if (p_driver == NULL) {
        return NRF_ERROR_NOT_SUPPORTED;
    }

    settings_data_t old_settings_data;

    err = p_driver->read_file_data(SETTINGS_FILE_NAME, &old_settings_data, sizeof(settings_data_t));
    bool not_found = false;
    if (err == VFS_ERR_NOOBJ) {
        not_found = true;
    } else if (err < 0) {
        return NRF_ERROR_INVALID_STATE;
    }

    if (not_found || memcmp(&m_settings_data, &old_settings_data, sizeof(settings_data_t)) != 0) {
        err = p_driver->write_file_data(SETTINGS_FILE_NAME, &m_settings_data, sizeof(settings_data_t));
        if (err < 0) {
            return NRF_ERROR_INVALID_STATE;
        }

        if (not_found) {
            vfs_meta_t meta;
            memset(&meta, 0, sizeof(meta));
            meta.has_flags = true;
            meta.flags = VFS_OBJ_FLAG_HIDDEN;

            uint8_t meta_data[VFS_MAX_META_LEN];
            vfs_meta_encode(meta_data, sizeof(meta_data), &meta);
            err = p_driver->update_file_meta(SETTINGS_FILE_NAME, &meta_data, sizeof(meta_data));
            NRF_LOG_INFO("Settings file meta updated!: %d", err);
        }

        NRF_LOG_INFO("settings saved!");
    }

    return NRF_SUCCESS;
}

settings_data_t *settings_get_data() { return &m_settings_data; }

int32_t settings_reset() {
    memcpy(&m_settings_data, &def_settings_data, sizeof(settings_data_t));
    vfs_driver_t *p_driver = vfs_get_default_driver();
    return p_driver->remove_file(SETTINGS_FILE_NAME);
}
