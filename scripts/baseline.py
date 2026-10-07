#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
全量基线提取器（BMS 项目专用）—— 从 build_all.log 算出断言总数。

为什么需要它（而不是手工数）：
  build_all.log 是**混合编码**的 —— .bat 的 echo 是 CP936，C++ 与 Python 子进程的
  stdout 是 UTF-8。整文件按单一编码解码**必漏一半**（实测：按 CP936 解码时 15/ 的
  「通过 125」变成乱码，正则匹配不上，总数少算 125）。所以这里**逐行**先试 UTF-8
  再退 CP936。

口径规则（"每步一项"）：
  · 每步取**末条** `PASS=n`；若该值小于同一步内各 exe 汇总行之和，则说明这一步
    **没有打汇总行**（13/ 打印 3 层、P3 打印 2 组、20/ 打印 3 个 exe 各自一行），
    此时取**去重后各 exe 汇总行之和**。
  · 步骤号集合必须恰好 1..N 无缺号（与 12/ 验收器 A7-04 同一条纪律）。

用法：
  python scripts/baseline.py                 # 读 build_all.log
  python scripts/baseline.py --log 其他.log
  python scripts/baseline.py --json          # 只印机器可读的汇总
退出码：0 正常；1 步骤号不连续或分母不一致。
"""
import argparse
import json
import re
import sys

# 一行里出现这些词，说明它是"这一步的汇总行"而不是某个 exe 的单独计数
TOTAL_HINTS = ("合计", "assertions total", "总计")


def read_mixed(path):
    """逐行解混合编码日志。返回 (文本, 行数)。"""
    out = []
    with open(path, "rb") as fh:
        for b in fh:
            b = b.rstrip(b"\r\n")
            for enc in ("utf-8", "cp936"):
                try:
                    out.append(b.decode(enc))
                    break
                except Exception:
                    continue
            else:
                out.append(b.decode("utf-8", "replace"))
    return "\n".join(out), len(out)


def split_steps(txt):
    marks = [(m.start(), int(m.group(1)), int(m.group(2)))
             for m in re.finditer(r"^(\d+)/(\d+) ", txt, re.M)]
    segs = []
    for i, (pos, n, d) in enumerate(marks):
        end = marks[i + 1][0] if i + 1 < len(marks) else len(txt)
        segs.append((n, d, txt[pos:end]))
    return segs


def step_values(seg):
    """取一步里所有"计数行"的值。

    ★ 三种打印格式都要认全（本项目实测）：
      · `PASS=n FAIL=m SKIPPED=k`  —— 大多数模块
      · `通过 n / 失败 m`          —— 07\\/P1/P2/15\\ 等（中文）
      · 只印 `ALL TESTS PASSED`    —— 02\\ 03\\，**没有计数**，本来就不该计入
    只认第一种会静默少算（实测按单一格式扫描会漏掉 15\\ 的 125）。
    """
    vals = [int(x) for x in re.findall(r"PASS=(\d+)", seg)]
    if not vals:
        vals = [int(x) for x in re.findall(r"通过\s*(\d+)", seg)]
    return vals


def step_count(seg):
    """按口径规则算一步的断言数；返回 (值, 说明)。"""
    lines = [l.strip() for l in seg.split("\n") if re.search(r"PASS=\d+|通过\s*\d+", l)]
    vals = step_values(seg)
    if not vals:
        return None, "无计数"
    last = vals[-1]
    if len(vals) == 1:
        return last, "单行"
    if any(h in l for l in lines for h in TOTAL_HINTS):
        return last, "取汇总行"
    # 末行是否就是"各 exe 之和"？是 ⇒ 它是汇总行但没有关键字（如 18/ 的 PASS=91）
    if sum(set(vals[:-1])) == last:
        return last, "末行即合计"
    # 否则这一步**没有打汇总行**，只能把各 exe 的计数加总（13/ P3 20/）
    uniq = sorted(set(vals))
    return sum(uniq), "加总 " + "+".join(str(u) for u in uniq)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="build_all.log")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()

    try:
        txt, nlines = read_mixed(a.log)
    except OSError as e:
        print("[FAIL] 读不到日志:", e)
        return 1

    segs = split_steps(txt)
    if not segs:
        print("[FAIL] 日志里找不到 `N/M ` 步骤横幅 —— 不是 build_all 的输出？")
        return 1

    denoms = sorted(set(d for _, d, _ in segs))
    steps = [n for n, _, _ in segs]
    items, total = [], 0
    for n, d, seg in segs:
        v, why = step_count(seg)
        items.append({"step": n, "count": v, "why": why})
        if v:
            total += v

    ok = (len(denoms) == 1 and steps == list(range(1, len(steps) + 1)))

    if a.json:
        print(json.dumps({"steps": len(steps), "denom": denoms[0] if denoms else None,
                          "counted": sum(1 for i in items if i["count"]),
                          "total": total, "consistent": ok}, ensure_ascii=False))
        return 0 if ok else 1

    print("源码行数 : %d" % nlines)
    print("步骤     : %d 步，分母 %s" % (len(steps), denoms))
    print("自洽     : %s" % ("是（1..N 无缺号）" if ok else "**否** —— 有缺号或分母混用"))
    print()
    print(" 步  断言数   取值口径")
    print("---- --------  ----------------------------")
    for it in items:
        print("%4d %8s  %s" % (it["step"], it["count"] if it["count"] else "—", it["why"]))
    print()
    print("有计数的步数 : %d" % sum(1 for i in items if i["count"]))
    print("基线合计     : %d" % total)
    if not ok:
        print("[FAIL] 步骤号不连续或分母不一致 —— 12/ 验收器 A7-04 会红")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
