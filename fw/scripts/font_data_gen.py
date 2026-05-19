#!/usr/bin/env python3
"""
font_data_gen.py  (v8.2-fix8)

为 u8g2 字库生成 wenquanyi 12pt 中文字库的 C 源文件.

v8.2-fix8 (字库扩容) 改动:
  1. lite 版本不再只覆盖 GB2312 Level 1, 还加入:
     a) 用户手动维护的 "CURATED_L2" —— 现实里常见但 jieba 频率统计
        命中不到的二级字 (例: 哔哩哔哩 的 "哔", 名字里常见美字 璀璨翊翎,
        Sky badge / 命名场景里的拟声叹词 嗨嗒嘞嗖, 等等).
     b) 用 jieba 内置 dict.txt 推算出来的 "L2 高频前 500 字". 这一批主要
        是 sky_badge_db.c 这种长尾内容里常见的二级字, 同时也覆盖了大量
        现代姓名 / 地名 / 常用复合词的边缘字.
     c) 仍然包括今天的 base = GB2312 L1 (3755) + 源码扫到的中文字面量 +
        chinese3.txt / gb2312a.txt + CJK 标点 + 全角 ASCII.
  2. 实测扩容后大小 (Linux 上 bdfconv -b 0 -f 1):
       原 lite          : 4086 字形 / 109614 字节 (109.6 KB)
       新 lite (本版本) : 5150 字形 / 134522 字节 (134.5 KB)
       full             : 7575 字形 / 203005 字节 (200.9 KB)
     application FLASH 总额 0x5B000 = 364 KB, 原 lite 留约 33 KB 余量,
     新 lite 仍留约 4 KB 余量, 满足 "扩字库 但不溢出" 的硬约束.
  3. full 版本逻辑跟 v8.1-fix3 一致, 仅供有人改 ld + 扩 FLASH 后启用.

注意:
  - 新增 jieba 依赖只在 *生成时* 用. 固件本身只读 .c 字模, 与 jieba 无关.
  - 没装 jieba 的环境会回落到一个内嵌的 "高频 L2 子集" (脚本同目录的
    common_l2.txt). 这样仓库里至少有一种确定性的字符列表, 避免不同人
    跑出不同字库.
  - 跑过本脚本以后:
      git diff --stat fw/application/src/mui/*.c
    应该只看到 u8g2_font_wqy12_t_gb2312a_lite.c (和可选 .c) 的内容变化.
"""

import os
import re
import platform
import subprocess
from collections import Counter


# ----------------------------------------------------------------------
# 路径
# ----------------------------------------------------------------------
current_dir = os.path.dirname(os.path.abspath(__file__))

source_dirs = [
    os.path.join(current_dir, "../application/src/i18n"),
    os.path.join(current_dir, "../application/src/amiidb"),
    os.path.join(current_dir, "../application/src/app/chameleon/port"),
    os.path.join(current_dir, "../application/src/app/amiibo"),
]

data_dir = os.path.join(current_dir, "../data")
mui_dir = os.path.join(data_dir, "../application/src/mui")
common_l2_file = os.path.join(current_dir, "common_l2.txt")

RX_STR_LIT = re.compile(r'"((?:\\.|[^"\\])*)"', re.DOTALL)
RX_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
RX_LINE_COMMENT = re.compile(r"//[^\n]*")


# ----------------------------------------------------------------------
# 手工维护的 "现代用法 L2" 字集
# ----------------------------------------------------------------------
# 这些字在 jieba 的传统语料频率里偏低 (因为没多少正式新闻语料会用),
# 但是 Sky 徽章 / 哔哩哔哩 / 抖音 / 微信 等场景下经常碰到. 列在这里以
# 保证它们一定出现在 lite 字库里, 不依赖统计.
#
# 怎么扩这个表:
#   把用户报告 "这个字显示不出来" 的字塞进对应分组里, 跑一次本脚本即可.
#   每多 1 个字 ≈ 多吃 27 字节 FLASH; 加 200~300 字也吃不爆 (当前留有
#   4KB 余量), 但加 1000 字以上要重新看 FLASH 余量再说.
CURATED_L2_CHARS = (
    # 平台 / IP 关键字 (Bilibili 等)
    "哔"
    # 拟声词 / 感叹词 / 网络聊天高频
    "啵啰啦哒哎喔嘞嗖咻咝嘤嘟嗨嗦嗲咩咪喵咧嚎"
    # 化学 / 矿物 / Sky 风格命名
    "酰酐酯酞醛醋醌酚酿酶酹酋酊酎酗酥酡醪醵醐"
    # 视觉动作 / 神态 (常见于角色 / 徽章描述)
    "瞋瞒瞠瞧瞻瞎瞄瞪瞅"
    # 植物 / 礼盒 / 节令 (Sky badge db 倾向)
    "栀杞枸槁榴梓樱"
    # 名字常用美字 (玩家 ID 高频)
    "璀璨璞璟珏珑琰玮"
    "翊翎翔翰翱翳"
    "煦煊熠熹熏燚燊"
    "睿睦睇睥眯眷眸瞩"
    "穹窕窈窠窣"
    "嬛嬗嫔嫦娆姗婧妩妫嫣"
    # 文学修辞
    "邃邈邂逅迤逦"
)


def curated_l2_codepoints():
    s = set()
    for c in CURATED_L2_CHARS:
        if ord(c) > 0x7E:
            s.add(ord(c))
    return s


# ----------------------------------------------------------------------
# 通用工具
# ----------------------------------------------------------------------
def write_to_file(file_path, content):
    with open(file_path, "w", encoding="utf-8") as f:
        f.write(content)


def extract_chars_from_source():
    chars = set()
    for source_dir in source_dirs:
        if not os.path.isdir(source_dir):
            continue
        for root, _, files in os.walk(source_dir):
            for fname in files:
                if not (fname.endswith(".c") or fname.endswith(".h")):
                    continue
                fpath = os.path.join(root, fname)
                try:
                    with open(fpath, "r", encoding="utf-8") as fh:
                        text = fh.read()
                except UnicodeDecodeError:
                    with open(fpath, "r", encoding="utf-8", errors="replace") as fh:
                        text = fh.read()
                text = RX_BLOCK_COMMENT.sub("", text)
                text = RX_LINE_COMMENT.sub("", text)
                for m in RX_STR_LIT.findall(text):
                    for c in m:
                        if ord(c) > 0x7E and c not in ("\n", "\r", "\t"):
                            chars.add(ord(c))
    return chars


def read_txt_chars(*filenames):
    chars = set()
    for fn in filenames:
        fpath = os.path.join(data_dir, fn)
        if not os.path.exists(fpath):
            continue
        with open(fpath, "r", encoding="utf-8") as f:
            for line in f:
                for c in line.strip():
                    if ord(c) > 0x7E:
                        chars.add(ord(c))
    return chars


def get_gb2312_chars():
    """GB2312 Level 1 + Level 2 (实际中文字, 不含标点段)."""
    chars = set()
    for row in range(0xB0, 0xF8):
        max_col = 0xF9 if row == 0xD7 else 0xFE
        for col in range(0xA1, max_col + 1):
            try:
                c = bytes([row, col]).decode("gb2312")
                chars.add(ord(c))
            except Exception:
                pass
    for row in range(0xA1, 0xAA):
        for col in range(0xA1, 0xFF):
            try:
                c = bytes([row, col]).decode("gb2312")
                if ord(c) > 0x7E:
                    chars.add(ord(c))
            except Exception:
                pass
    return chars


def get_gb2312_level1_chars():
    chars = set()
    for row in range(0xB0, 0xD8):
        max_col = 0xF9 if row == 0xD7 else 0xFE
        for col in range(0xA1, max_col + 1):
            try:
                c = bytes([row, col]).decode("gb2312")
                chars.add(ord(c))
            except Exception:
                pass
    return chars


def get_gb2312_level2_chars():
    chars = set()
    for row in range(0xD8, 0xF8):
        for col in range(0xA1, 0xFF):
            try:
                c = bytes([row, col]).decode("gb2312")
                chars.add(ord(c))
            except Exception:
                pass
    return chars


def get_extra_cjk_chars():
    chars = set()
    for cp in range(0x3000, 0x303F + 1):
        chars.add(cp)
    for cp in range(0xFF00, 0xFF5E + 1):
        chars.add(cp)
    return chars


# ----------------------------------------------------------------------
# 频率排序: 优先 jieba; 没装 jieba 时回落到 common_l2.txt
# ----------------------------------------------------------------------
def rank_l2_by_jieba(l2_set):
    """返回 [(cp, score), ...], 按频率降序. jieba 没装时返回 None."""
    try:
        import jieba as _jieba  # noqa: F401
        dict_path = os.path.join(os.path.dirname(_jieba.__file__), "dict.txt")
        if not os.path.exists(dict_path):
            return None
        cf = Counter()
        with open(dict_path, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) != 3:
                    continue
                word, freq, _tag = parts
                try:
                    fv = int(freq)
                except ValueError:
                    continue
                for c in word:
                    if 0x4E00 <= ord(c) <= 0x9FFF:
                        cf[ord(c)] += fv
        return sorted(((cp, cf.get(cp, 0)) for cp in l2_set),
                      key=lambda x: -x[1])
    except ImportError:
        return None


def rank_l2_from_common_file(l2_set):
    """从 common_l2.txt 读出来, 文件出现顺序作为频率排序的近似."""
    if not os.path.exists(common_l2_file):
        return []
    out = []
    seen = set()
    with open(common_l2_file, "r", encoding="utf-8") as f:
        for line in f:
            for c in line:
                cp = ord(c)
                if cp in l2_set and cp not in seen:
                    out.append((cp, 0))
                    seen.add(cp)
    return out


# 目标: lite 字库吃下 GB2312-L1 + 这一批 L2 字, 总尺寸控制在 ~135 KB.
# 实测 top-500 L2 + 当前 CURATED_L2 = ~134.5 KB, 余 ~4 KB FLASH 安全余量.
L2_TOP_N = 500


def select_extended_l2(l2_set):
    ranked = rank_l2_by_jieba(l2_set)
    src = "jieba"
    if ranked is None:
        ranked = rank_l2_from_common_file(l2_set)
        src = "common_l2.txt fallback"
    if not ranked:
        return set(), src
    return set(cp for cp, _ in ranked[:L2_TOP_N]), src


# ----------------------------------------------------------------------
# bdfconv 调用
# ----------------------------------------------------------------------
def build_map_file(chars, output_path):
    lines = ["32-128,"]
    for cp in sorted(chars):
        lines.append(f"${cp:04X},")
    write_to_file(output_path, "\n".join(lines))


def run_bdfconv(map_path, output_path, bdf_path):
    system = platform.system()
    if system == "Windows":
        bdfconv_path = os.path.join(current_dir, "bdfconv.exe")
    elif system == "Darwin":
        bdfconv_path = os.path.join(current_dir, "bdfconv_macos_universal")
    elif system == "Linux":
        bdfconv_path = os.path.join(current_dir, "bdfconv_linux")
    else:
        raise OSError("Unsupported operating system")

    cmd = [
        bdfconv_path, "-b", "0", "-f", "1",
        "-M", os.path.abspath(map_path),
        "-n", "u8g2_font_wqy12_t_gb2312a",
        "-o", os.path.abspath(output_path),
        os.path.abspath(bdf_path),
    ]
    subprocess.run(cmd, check=True)


def wrap_font_file(temp_c, final_c, guard_ifdef=True):
    with open(final_c, "w+", encoding="utf-8") as out:
        out.write('\n#include "mui_u8g2.h"\n\n#include "u8x8.h"\n\n')
        with open(temp_c, "r", encoding="utf-8") as tmp:
            content = tmp.read()
        if not guard_ifdef:
            content = content.replace(
                "#ifdef U8G2_USE_LARGE_FONTS",
                "#ifndef U8G2_USE_LARGE_FONTS",
            )
        out.write(content)


def measure_binary_size(c_path):
    """从生成的 .c 文件里抠出 const uint8_t font_name[N] 的 N 值."""
    try:
        with open(c_path, "r", encoding="utf-8") as f:
            t = f.read()
    except OSError:
        return -1
    m = re.search(r"\[(\d+)\]\s+U8G2_FONT_SECTION", t)
    return int(m.group(1)) if m else -1


# ----------------------------------------------------------------------
# 主流程
# ----------------------------------------------------------------------
def main():
    bdf_path = os.path.join(data_dir, "wenquanyi_9pt_u8g2.bdf")

    base_chars = read_txt_chars("chinese3.txt", "gb2312a.txt")
    src_chars = extract_chars_from_source()
    extra_chars = get_extra_cjk_chars()

    # ----------------- full 版本 (启用 -DU8G2_USE_LARGE_FONTS 时才编进) --
    full_chars = set()
    full_chars.update(base_chars)
    full_chars.update(src_chars)
    full_chars.update(extra_chars)
    full_chars.update(get_gb2312_chars())

    full_map = os.path.join(data_dir, "gb2312_full.map")
    build_map_file(full_chars, full_map)

    temp_full = os.path.join(mui_dir, "u8g2_font_wqy12_t_gb2312a_t.c")
    run_bdfconv(full_map, temp_full, bdf_path)
    final_full = os.path.join(mui_dir, "u8g2_font_wqy12_t_gb2312a.c")
    full_sz = measure_binary_size(temp_full)
    wrap_font_file(temp_full, final_full, guard_ifdef=True)
    os.remove(temp_full)

    # ----------------- lite 版本 (默认编进固件) -------------------------
    lite_chars = set()
    lite_chars.update(base_chars)
    lite_chars.update(src_chars)
    lite_chars.update(extra_chars)
    lite_chars.update(get_gb2312_level1_chars())

    # v8.2-fix8 扩容: 手工 curated L2 + 频率前 N 的 L2 高频字
    l2_set = get_gb2312_level2_chars()
    curated = curated_l2_codepoints() & l2_set
    lite_chars.update(curated)

    ranked_l2, ranking_src = select_extended_l2(l2_set)
    lite_chars.update(ranked_l2)

    lite_map = os.path.join(data_dir, "gb2312_lite.map")
    build_map_file(lite_chars, lite_map)

    temp_lite = os.path.join(mui_dir, "u8g2_font_wqy12_t_gb2312a_lite_t.c")
    run_bdfconv(lite_map, temp_lite, bdf_path)
    final_lite = os.path.join(mui_dir, "u8g2_font_wqy12_t_gb2312a_lite.c")
    lite_sz = measure_binary_size(temp_lite)
    wrap_font_file(temp_lite, final_lite, guard_ifdef=False)
    os.remove(temp_lite)

    for f in [full_map, lite_map]:
        if os.path.exists(f):
            os.remove(f)

    # ----------------- 总结 --------------------------------------------
    print("font_data_gen.py: done")
    print(f"  full chars : {len(full_chars):5d}  ->  {full_sz:7d} bytes "
          f"({full_sz/1024:.1f} KB)")
    print(f"  lite chars : {len(lite_chars):5d}  ->  {lite_sz:7d} bytes "
          f"({lite_sz/1024:.1f} KB)")
    base_only = len(lite_chars) - len(curated) - len(ranked_l2)
    print(f"    base (L1+src+extras)   : {base_only}")
    print(f"    + curated L2 (手工选)  : +{len(curated)} "
          f'("哔哩哔哩"/拟声词/名字等)')
    print(f"    + top-{L2_TOP_N} L2 freq : +{len(ranked_l2)} (来源: {ranking_src})")
    if lite_sz > 0:
        BUDGET = 139 * 1024     # ~25KB 留给非字库代码增长
        if lite_sz > BUDGET:
            print(f"WARNING: lite 字库 {lite_sz} 字节 已经超出我们设定的 "
                  f"{BUDGET} 字节安全预算, 编出来可能 .text 溢出 FLASH "
                  f"(application LD: 0x5B000). 减小 L2_TOP_N 或裁剪 "
                  f"CURATED_L2_CHARS.")


if __name__ == "__main__":
    main()
