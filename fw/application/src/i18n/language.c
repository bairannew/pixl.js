
#include "language.h"

/*
 * v8.1-fix2: 多语言瘦身后的语言派发表.
 *
 * 只保留中文简体 / 中文繁体两套. fallback 也改成简体 (原版 fallback 到 lang_en_US,
 * 但 en_US.c 已删).
 */

typedef struct {
    const char **strings;
} LanguageData;

const LanguageData const languageData[LANGUAGE_COUNT] = {
    [LANGUAGE_ZH_HANS] = {.strings = lang_zh_Hans},
    [LANGUAGE_ZH_TW]   = {.strings = lang_zh_TW},
};

// 当前语言设置 (Current language setting)
Language currentLanguage = LANGUAGE_ZH_HANS;

const char *getLangString(L_StringID stringID) {
    if (stringID >= _L_COUNT) {
        return "@@STR@@";
    }
    if (currentLanguage >= LANGUAGE_COUNT) {
        /* v8.1-fix2: 老 settings.bin 可能存了 en/ja 这类枚举值,
         * 这里兜底回简体而不是英文 (英文表已删). */
        return lang_zh_Hans[stringID];
    }
    const char *string = languageData[currentLanguage].strings[stringID];
    /* 该语言下这条 key 没填 → 回简体 */
    return string && strlen(string) > 0 ? string : lang_zh_Hans[stringID];
}

void setLanguage(Language lang) { currentLanguage = lang; }

const char *getLangDesc(Language lang) {
    switch (lang) {
        case LANGUAGE_ZH_HANS:
            return "简体中文";
        case LANGUAGE_ZH_TW:
            return "繁體中文(臺灣)";
        default:
            return "@@LANG@@";
    }
}

Language getLanguage() { return currentLanguage; }
