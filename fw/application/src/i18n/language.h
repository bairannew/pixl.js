#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef LANGUAGE_H
#define LANGUAGE_H

#include "string_id.h"

#define _T(x) getLangString(_L_##x)


/*
 * v8.1-fix2: 语言列表瘦身.
 *
 * 用户要求只保留中文简体 / 中文繁体两套, 其它(en/ja/de/es/fr/hu/it/nl/pl/pt_BR/pt_PT/ru/sv)
 * 整套 lang_<xx>[] 字符串表和对应的 .c 文件已经删除. 这样能省下
 * 大约 12 * 700 ≈ 8.4k 行只读字符串数据, 大幅减小 ROM/Flash 占用.
 *
 * 枚举顺序: ZH_HANS = 0 (默认), ZH_TW = 1. 兼容老 settings.bin 里
 * 已经存了枚举值 0~14 的情况: 0 仍然映射到简体, 1~14 都被 validate_settings()
 * 强制回退到 LANGUAGE_ZH_HANS (除了 2 这个旧 ZH_TW 值, validate 里会改成 1).
 */
typedef enum {
    LANGUAGE_ZH_HANS = 0,
    LANGUAGE_ZH_TW   = 1,
    LANGUAGE_COUNT
} Language;

extern const char* lang_zh_Hans[_L_COUNT];
extern const char* lang_zh_TW[_L_COUNT];

// 获取字符串的函数 (Get language string function)
const char* getLangString(L_StringID stringID);
void setLanguage(Language lang);
Language getLanguage();
const char* getLangDesc(Language lang);


#endif // LANGUAGE_H
