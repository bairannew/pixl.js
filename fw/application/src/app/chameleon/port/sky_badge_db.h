/*
 * sky_badge_db.h
 *
 * 光遇 (Sky: Children of the Light) 实体徽章数据库 (105 枚).
 *
 * 数据来源: Star Android 项目 (brr.star.sky.app.tool.BadgeRegistry +
 *           values-zh/strings.xml).
 *
 * 每条徽章记录:
 *   sk        - 徽章唯一标识 (例如 "SKY-PN-ST-POR-HF"), Sky 官方 NTAG 链接里
 *               base64 解码后的 sk= 参数内容
 *   idx       - Star 项目内部编号 (1..105), 仅作展示/调试用
 *   name_zh   - 中文徽章名称 (UTF-8) - 会自动填到当前卡槽的 "卡名/徽章名"
 *   note_zh   - 中文备注 (UTF-8), Star 项目里是冷却时间, 用作 nickname 的兜底
 *
 * 二次编写约定:
 *   - 调用方拿到 entry 后, 应当先用 name_zh 作为新昵称写入 vfs_meta.notes,
 *     用户可以再进入 "设置徽章名" 场景手动改写.
 *   - 如果 sk 未在表中查到, 调用方应保留用户当前的昵称, 不要覆盖.
 *
 * 编辑说明:
 *   表里有重复 idx 没关系 (Star 项目里有同一图标对应多个 sk 的情况), 我们按
 *   sk 唯一性建表; sk 查找用线性扫描足够 (105 条 * 短字符串比较).
 *
 *  ─ 光遇徽章定制版 (Sky Badge Edition) - sky_badge_db ─
 */
#ifndef SKY_BADGE_DB_H
#define SKY_BADGE_DB_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 单条徽章条目. 所有字符串都是 UTF-8 (中文), 直接可写进 vfs_meta.notes. */
typedef struct {
    const char *sk;       /* 徽章 sk 字符串 (例如 "SKY-PN-ST-POR-HF") */
    uint16_t    idx;      /* Star 项目内的编号 (展示用) */
    const char *name_zh;  /* 中文名称, UTF-8 */
    const char *note_zh;  /* 中文备注 (冷却时间等), UTF-8 */
} sky_badge_entry_t;

/** 全表. 见 sky_badge_db.c. */
extern const sky_badge_entry_t sky_badge_db[];
extern const size_t sky_badge_db_count;

/**
 * 按 sk 精确匹配查询徽章条目.
 *
 * @param sk    待查 sk 字符串 (非 NULL).
 * @return      命中返回对应条目指针 (静态生命周期, 不要 free); 没命中返回 NULL.
 */
const sky_badge_entry_t *sky_badge_db_find(const char *sk);

#ifdef __cplusplus
}
#endif

#endif /* SKY_BADGE_DB_H */
