#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""19/ 日志落盘与轮转 —— 把 P2 的有界 SOE / metrics 接成"能长期留、能检索"。

背景（`docs/交接/下一步工作交接说明.md` §3.3 第 3 条）
------------------------------------------------------
> P2 的 `SoeLog` 默认有界（4096 条 ≈ 600 KB），长期运行必须外送或落盘。

**这不是"加个文件就完了"的问题，是有明确失效模式的问题**：`SoeLog` 超出
`capacity` 就**淘汰最旧**并累加 `dropped()`。也就是说 ——

    导出周期太长 → 两次导出之间生成的事件数 > 4096 → 最旧的事件**永久消失**，
    而且现象极具欺骗性：段里"有事件"、条数"正常"、程序"没报错"。

所以本文件解决两件事，缺一不可：

① **落盘与轮转**：定期把导出的批次追加进带轮转的存储（大小 + 条数双阈值、
   保留 N 份、删最旧），并支持按时间范围把旧事件捞回来。
② **接漏**：批次自带的 `summary.dropped` 一旦非 0，就**大声报**出来 ——
   掉过事件这件事必须留痕，否则"日志里没看到故障"会被误读成"当时没故障"。

不丢事件的算式（现场必须按它设导出周期）
----------------------------------------
设 `r` = 事件最大生成速率（条/秒，取**风暴峰值**不是均值）、`T` = 导出周期（秒）、
`C` = `SoeLog::capacity`（默认 4096）：

    T × r < C          ← 不丢事件的充要条件（严格小于；等于时不安全）

现场留 2 倍余量：`T ≤ C / (2r)`。举例（`C = 4096`）：

| 最大事件速率 r | 不丢事件的最大周期 T | 建议设置（2 倍余量） |
| --- | --- | --- |
| 1 条/s（常态） | 4096 s ≈ 68 min | 30 min |
| 10 条/s | 409.6 s | 200 s |
| 100 条/s（风暴） | 40.96 s | 20 s |

**注意 `r` 的实际量级远小于"拍率"**：P2 的核心机制是边沿检测 + 时间窗抑制，
一次持续 838 拍的安全限幅只产出 **2 条**事件（`SAFETY_CLIP_START/END`）。
所以现场真实的 `r` 由**事件的跳变次数**决定，不由 10 Hz 的控制周期决定。
`advise` 子命令把这个算式做成可执行的：

    python log_rotator.py --root <root> advise --rate 100 --capacity 4096
    → 最大周期 40.96 s，建议 ≤ 20.48 s

轮转的双阈值与"什么算一条"
--------------------------
| 类别 | 一条 = | 默认条数阈值 | 默认大小阈值 |
| --- | --- | --- | --- |
| `soe` | 一个事件行的 CSV 行 | 2000 条 | 512 KiB |
| `metrics` | 一次导出（一整块快照） | 200 块 | 256 KiB |

任一阈值超出即**封段**（rename 成序号名）+ 开新段，然后按 `keep` 删最旧的封存段。
`keep × 阈值` 就是**保留窗口**：窗口外的历史会被删掉 —— 这是设计意图，不是缺陷
（磁盘不会无限大）。要看更早的历史，必须先把段归档到别处（见 `docs/README.md` §已知边界）。

落盘约定
--------
```
<安装根>/log/
  soe/
    soe_current.csv          ← 正在追加
    soe_0001.csv …           ← 已封存（序号即时间序）
  metrics/
    metrics_current.prom
    metrics_0001.prom …
```

命令
----
    python log_rotator.py --root <root> ingest --csv <f> [--json <f>] [--interval S]
    python log_rotator.py --root <root> ingest-metrics <f> [--label T]
    python log_rotator.py --root <root> rotate [--why T]
    python log_rotator.py --root <root> status
    python log_rotator.py --root <root> query [--from T0] [--to T1]
                                  [--level L] [--code C] [--source S] [--json]
    python log_rotator.py --root <root> advise --rate R [--interval T] [--capacity C]

退出码：0 正常 / 1 用法或 IO 错误 / 2 检出事件丢失（`dropped > 0`）。
"""

from __future__ import annotations

import argparse
import csv
import datetime
import io
import json
import os
import re
import sys

# SOE CSV 的列（= P2 `SoeLog::csv_header()`）。**列名即契约**，改了这里就必须
# 同步改 P2 的导出，因此本模块把这句话做成一条会红的断言（见 tests）。
SOE_COLUMNS = ["t_first", "t_last", "duration_s", "repeat",
               "level", "source", "code", "message", "data"]
SOE_HEADER = ",".join(SOE_COLUMNS)

LEVELS = ["DEBUG", "INFO", "WARN", "ERROR", "FATAL"]

# 时间比较容差（秒）。1 µs —— 远小于任何事件分辨率，又足以吸收浮点累积误差。
#
# ★ 为什么必须有它（实测踩到的）：事件时间戳是 `i * 0.1` 这样累出来的，
#   二进制浮点下 `19 * 0.1 == 1.9000000000000001`。于是"查 [0.0, 1.9]"
#   会把第 19 条**排除掉** —— 运维看到的是"边界上那一条不见了"，
#   而它既不是丢失也不是查询写错，纯粹是 `0.1` 不能精确表示。
#   日志检索工具在这件事上"精确"是错的：用户写 1.9 指的就是那一条。
#   （不加这个容差的实现见 tests 的反向守卫：会少 1 条。）
T_EPS_S = 1e-6

SEG_RE = re.compile(r"^([a-z_]+)_(\d+)$")
DEFAULTS = {
    "soe":     {"max_bytes": 512 * 1024, "max_entries": 2000, "ext": "csv"},
    "metrics": {"max_bytes": 256 * 1024, "max_entries": 200,  "ext": "prom"},
}
DEFAULT_KEEP = 5


def _now_iso() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


class Event:
    __slots__ = ("t_first", "t_last", "duration_s", "repeat", "level",
                 "source", "code", "message", "data")

    def __init__(self, t_first: float, t_last: float, duration_s: float,
                 repeat: int, level: str, source: str, code: str,
                 message: str, data: str) -> None:
        self.t_first = t_first
        self.t_last = t_last
        self.duration_s = duration_s
        self.repeat = repeat
        self.level = level
        self.source = source
        self.code = code
        self.message = message
        self.data = data

    @property
    def level_rank(self) -> int:
        try:
            return LEVELS.index(self.level)
        except ValueError:
            return 0

    def key(self) -> tuple:
        return (self.t_first, self.level, self.source, self.code, self.message, self.data)

    def as_row(self) -> list[str]:
        return [repr(self.t_first), repr(self.t_last), repr(self.duration_s),
                str(self.repeat), self.level, self.source, self.code,
                self.message, self.data]

    def to_csv(self) -> str:
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator="\n")
        w.writerow(self.as_row())
        return buf.getvalue()

    def to_dict(self) -> dict:
        return {
            "t_first": self.t_first, "t_last": self.t_last,
            "duration_s": self.duration_s, "repeat": self.repeat,
            "level": self.level, "source": self.source, "code": self.code,
            "message": self.message, "data": self.data,
        }


def parse_soe_csv(text: str) -> list[Event]:
    """解析 P2 导出的 SOE CSV。

    容忍三种非事件行，全部跳过（它们不是"坏数据"，是**有意写进去的**）：
      · 表头行（首列 == `t_first`）
      · 轮转标记行（首列以 `#` 开头）
      · 列数不足 9 的行（截断/空行）
    ★ 不"尽量猜"：列数不对却硬解析，会把一行坏数据变成一条看似合法的事件。
    """
    events: list[Event] = []
    rdr = csv.reader(io.StringIO(text))
    for row in rdr:
        if not row:
            continue
        if row[0].startswith("#") or row[0] == "t_first":
            continue
        if len(row) != len(SOE_COLUMNS):
            continue
        try:
            t_first = float(row[0])
            t_last = float(row[1])
            duration_s = float(row[2])
            repeat = int(float(row[3]))
        except ValueError:
            continue
        events.append(Event(t_first, t_last, duration_s, repeat,
                            row[4], row[5], row[6], row[7], row[8]))
    return events


class Rotator:
    def __init__(self, root: str, kind: str,
                 max_bytes: int | None = None,
                 max_entries: int | None = None,
                 keep: int = DEFAULT_KEEP) -> None:
        if kind not in DEFAULTS:
            raise ValueError(f"未知类别：{kind!r}（可用 soe / metrics）")
        d = DEFAULTS[kind]
        self.root = os.path.abspath(root)
        self.kind = kind
        self.ext = d["ext"]
        self.max_bytes = int(max_bytes if max_bytes is not None else d["max_bytes"])
        self.max_entries = int(max_entries if max_entries is not None else d["max_entries"])
        self.keep = int(keep)
        self.dir = os.path.join(self.root, "log", kind)
        self.current = os.path.join(self.dir, f"{kind}_current.{self.ext}")
        # 本次进程内累计（跨进程的账在文件里；这里只是本进程的观测）
        self.dropped_seen = 0
        self.last_interval_s: float | None = None
        self.last_suppressed: int | None = None

    # ---- 目录与段 ----

    def ensure_dirs(self) -> None:
        os.makedirs(self.dir, exist_ok=True)

    def sealed(self) -> list[tuple[int, str]]:
        if not os.path.isdir(self.dir):
            return []
        out = []
        for name in os.listdir(self.dir):
            stem, ext = os.path.splitext(name)
            m = SEG_RE.match(stem)
            if m and m.group(1) == self.kind and name.endswith("." + self.ext):
                out.append((int(m.group(2)), os.path.join(self.dir, name)))
        out.sort()
        return out

    def _write_block_header(self, reason: str, extra: str = "") -> str:
        ts = _now_iso()
        line = f"# batch {ts} reason={reason}{extra}\n"
        if self.kind == "soe":
            line += SOE_HEADER + "\n"
        return line

    def _ensure_current(self, reason: str = "open") -> None:
        self.ensure_dirs()
        if not os.path.exists(self.current):
            with open(self.current, "w", encoding="utf-8", newline="\n") as f:
                f.write(self._write_block_header(reason))

    def current_bytes(self) -> int:
        return os.path.getsize(self.current) if os.path.isfile(self.current) else 0

    def _current_event_lines(self) -> int:
        """当前段里"一条"的条数：soe 数事件行，metrics 数 `# batch` 块。"""
        if not os.path.isfile(self.current):
            return 0
        n = 0
        with open(self.current, "r", encoding="utf-8", newline="") as f:
            for line in f:
                if self.kind == "soe":
                    if line.startswith("#") or line.startswith("t_first"):
                        continue
                    if line.strip():
                        n += 1
                else:
                    if line.startswith("# batch"):
                        n += 1
        return n

    def stats(self) -> dict:
        return {
            "kind": self.kind,
            "dir": self.dir,
            "sealed_files": len(self.sealed()),
            "keep": self.keep,
            "current_bytes": self.current_bytes(),
            "current_entries": self._current_event_lines(),
            "max_bytes": self.max_bytes,
            "max_entries": self.max_entries,
            "dropped_seen": self.dropped_seen,
        }

    # ---- 轮转 ----

    def rotate(self, why: str = "manual") -> list[str]:
        """封段 + 开新段 + 按 keep 删最旧。返回本次新封存的段名。"""
        self.ensure_dirs()
        sealed_now: list[str] = []
        if os.path.isfile(self.current) and self.current_bytes() > 0:
            seq = (self.sealed()[-1][0] + 1) if self.sealed() else 1
            target = os.path.join(self.dir, f"{self.kind}_{seq:04d}.{self.ext}")
            os.replace(self.current, target)
            sealed_now.append(os.path.basename(target))
        # 删最旧：超出 keep 的封存段
        removed = self._enforce_retention()
        # 新段先写块头（这样"为什么封段"留在文件里，不是只留在内存）
        reason = why + (f" removed={','.join(removed)}" if removed else "")
        self._write_new_current(reason)
        return sealed_now

    def _write_new_current(self, reason: str) -> None:
        with open(self.current, "w", encoding="utf-8", newline="\n") as f:
            f.write(self._write_block_header(reason))

    def _enforce_retention(self) -> list[str]:
        segs = self.sealed()
        removed: list[str] = []
        while len(segs) > self.keep:
            _, path = segs.pop(0)
            try:
                os.remove(path)
                removed.append(os.path.basename(path))
            except OSError:
                break
        return removed

    def _over_threshold(self) -> str:
        if self.current_bytes() > self.max_bytes:
            return f"bytes={self.current_bytes()}>{self.max_bytes}"
        if self._current_event_lines() > self.max_entries:
            return f"entries={self._current_event_lines()}>{self.max_entries}"
        return ""

    # ---- 写入 ----

    def ingest_soe(self, csv_path: str | None = None, text: str | None = None,
                   json_path: str | None = None,
                   interval_s: float | None = None) -> dict:
        """把一批 SOE 追加进存储。

        `json_path` 给了就读 `summary.dropped` —— 那是**上一批是否已经掉过事件**
        的唯一证据。见文件头"接漏"。
        """
        if text is None:
            if not csv_path:
                raise ValueError("ingest 需要 --csv 或 text")
            with open(csv_path, "r", encoding="utf-8-sig", newline="") as f:
                text = f.read()

        events = parse_soe_csv(text)
        self._ensure_current("open")
        # 直接搬运原文（不做重排版）—— 保真优先：现场要能拿原始批次对账
        body = text if text.endswith("\n") or text == "" else text + "\n"
        with open(self.current, "a", encoding="utf-8", newline="\n") as f:
            f.write(body)

        dropped = 0
        suppressed = None
        if json_path:
            try:
                with open(json_path, "r", encoding="utf-8-sig") as f:
                    d = json.load(f)
                s = d.get("summary", {}) if isinstance(d, dict) else {}
                dropped = int(s.get("dropped", 0) or 0)
                if s.get("suppressed") is not None:
                    suppressed = int(s.get("suppressed") or 0)
            except (OSError, ValueError, TypeError):
                dropped = 0
        self.dropped_seen += dropped
        self.last_interval_s = interval_s
        self.last_suppressed = suppressed

        why = self._over_threshold()
        sealed = self.rotate(why or "threshold") if why else []

        return {
            "appended": len(events),
            "dropped_in_batch": dropped,
            "suppressed_in_batch": suppressed,
            "rotated": bool(sealed),
            "rotated_why": why,
            "sealed": sealed,
            "current_bytes": self.current_bytes(),
            "current_entries": self._current_event_lines(),
            "sealed_files": len(self.sealed()),
        }

    def ingest_metrics(self, path: str | None = None, text: str | None = None,
                       label: str = "") -> dict:
        if text is None:
            if not path:
                raise ValueError("ingest-metrics 需要 --file 或 text")
            with open(path, "r", encoding="utf-8-sig", newline="") as f:
                text = f.read()
        self._ensure_current("open")
        extra = f" label={label}" if label else ""
        with open(self.current, "a", encoding="utf-8", newline="\n") as f:
            f.write(self._write_block_header("metrics", extra))
            f.write(text if text.endswith("\n") or text == "" else text + "\n")
        why = self._over_threshold()
        sealed = self.rotate(why or "threshold") if why else []
        return {
            "blocks_appended": 1,
            "rotated": bool(sealed),
            "rotated_why": why,
            "sealed": sealed,
            "current_bytes": self.current_bytes(),
            "current_entries": self._current_event_lines(),
            "sealed_files": len(self.sealed()),
        }

    # ---- 检索 ----

    def all_segments(self) -> list[str]:
        return [p for _, p in self.sealed()] + \
               ([self.current] if os.path.isfile(self.current) else [])

    def query(self, t0: float | None = None, t1: float | None = None,
              level: str | None = None, code: str | None = None,
              source: str | None = None) -> list[Event]:
        """按时间范围 + 可选维度过滤。**已封存的段也一起扫** —— 这才是
        "轮转之后还能查到旧事件"的落地方式。"""
        min_rank = LEVELS.index(level) if level in LEVELS else None
        lo = None if t0 is None else t0 - T_EPS_S
        hi = None if t1 is None else t1 + T_EPS_S
        out: list[Event] = []
        for path in self.all_segments():
            with open(path, "r", encoding="utf-8", newline="") as f:
                for ev in parse_soe_csv(f.read()):
                    # 时间范围用"区间相交"：跨窗口的合并事件（START..END）
                    # 只要与 [t0,t1] 有重叠就该被查到，否则"故障持续期间"的检索
                    # 会漏掉那条事件（它的 t_first 在窗口之前）。
                    if lo is not None and ev.t_last < lo:
                        continue
                    if hi is not None and ev.t_first > hi:
                        continue
                    if min_rank is not None and ev.level_rank < min_rank:
                        continue
                    if code and ev.code != code:
                        continue
                    if source and ev.source != source:
                        continue
                    out.append(ev)
        out.sort(key=lambda e: (e.t_first, e.code))
        return out

    def earliest_last(self) -> tuple[float | None, float | None]:
        evs = self.query()
        if not evs:
            return None, None
        return evs[0].t_first, evs[-1].t_last


# =====================================================================
# 导出周期建议（把文件头的算式做成可执行的）
# =====================================================================
def advise(rate_per_s: float, interval_s: float | None = None,
           capacity: int = 4096) -> dict:
    if rate_per_s <= 0:
        raise ValueError("--rate 必须 > 0（事件/秒，取风暴峰值）")
    max_interval = capacity / rate_per_s
    rec = max_interval / 2.0
    res = {
        "rate_per_s": rate_per_s,
        "capacity": capacity,
        "max_interval_s": max_interval,
        "recommended_interval_s": rec,
    }
    if interval_s is not None:
        used = interval_s * rate_per_s
        res["interval_s"] = interval_s
        res["expected_events"] = used
        res["safe"] = used < capacity
        res["margin"] = capacity - used
    return res


def _fmt_advise(r: dict) -> str:
    lines = [
        f"  事件速率        : {r['rate_per_s']:g} 条/s",
        f"  SoeLog capacity : {r['capacity']} 条",
        f"  不丢事件最大周期: {r['max_interval_s']:.2f} s",
        f"  建议周期(2倍余量): {r['recommended_interval_s']:.2f} s",
    ]
    if "interval_s" in r:
        verdict = "不丢事件" if r["safe"] else "★ 会丢事件 ★"
        lines.append(f"  当前周期        : {r['interval_s']:g} s "
                     f"→ 预计 {r['expected_events']:.1f} 条/批 → {verdict}"
                     f"（余量 {r['margin']:.1f} 条）")
    return "\n".join(lines)


# =====================================================================
# CLI
# =====================================================================
def _rot(args) -> Rotator:
    return Rotator(args.root, args.kind, max_bytes=args.max_bytes,
                   max_entries=args.max_entries, keep=args.keep)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="19/ 日志落盘与轮转")
    ap.add_argument("--root", required=True, help="安装根目录")
    ap.add_argument("--kind", default="soe", choices=["soe", "metrics"])
    ap.add_argument("--max-bytes", type=int, default=None)
    ap.add_argument("--max-entries", type=int, default=None)
    ap.add_argument("--keep", type=int, default=DEFAULT_KEEP)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_in = sub.add_parser("ingest", help="追加一批 SOE 导出")
    p_in.add_argument("--csv", default=None)
    p_in.add_argument("--json", dest="json_path", default=None)
    p_in.add_argument("--interval", type=float, default=None,
                      help="本批与上批的间隔秒数（用于状态行的安全余量）")

    p_m = sub.add_parser("ingest-metrics", help="追加一次 metrics 导出")
    p_m.add_argument("--file", dest="file_path", default=None)
    p_m.add_argument("--label", default="")

    p_r = sub.add_parser("rotate", help="手工封段")
    p_r.add_argument("--why", default="manual")

    sub.add_parser("status", help="存储状态")

    p_q = sub.add_parser("query", help="按时间范围检索")
    p_q.add_argument("--from", dest="t0", type=float, default=None)
    p_q.add_argument("--to", dest="t1", type=float, default=None)
    p_q.add_argument("--level", default=None)
    p_q.add_argument("--code", default=None)
    p_q.add_argument("--source", default=None)
    p_q.add_argument("--json", dest="as_json", action="store_true")

    p_a = sub.add_parser("advise", help="导出周期建议（不丢事件算式）")
    p_a.add_argument("--rate", type=float, required=True)
    p_a.add_argument("--interval", type=float, default=None)
    p_a.add_argument("--capacity", type=int, default=4096)

    args = ap.parse_args(argv)

    if args.cmd == "advise":
        try:
            r = advise(args.rate, args.interval, args.capacity)
        except ValueError as e:
            print(f"[FAIL] {e}")
            return 1
        print("== 不丢事件算式：导出周期 × 最大事件速率 < SoeLog::capacity ==")
        print(_fmt_advise(r))
        return 0 if r.get("safe", True) else 2

    rt = _rot(args)

    if args.cmd == "ingest":
        try:
            res = rt.ingest_soe(csv_path=args.csv, json_path=args.json_path,
                                interval_s=args.interval)
        except (OSError, ValueError) as e:
            print(f"[FAIL] {e}")
            return 1
        print(f"[INGEST OK] 追加 {res['appended']} 条"
              f"（当前段 {res['current_entries']} 条 / {res['current_bytes']} 字节）")
        if res["rotated"]:
            print(f"  [ROTATE] 触发轮转 {res['rotated_why']} → 封存 {res['sealed']}"
                  f"，现存封存段 {res['sealed_files']} 个")
        if res["suppressed_in_batch"] is not None:
            print(f"  本批抑制合并 {res['suppressed_in_batch']} 次"
                  f"（P2 时间窗机制，不是丢失）")
        if res["dropped_in_batch"] > 0:
            print(f"  [LOST] ★ 本批导出时 SoeLog 已淘汰 {res['dropped_in_batch']} 条 ——"
                  f" 导出周期过长，事件已永久丢失")
            print(f"         按算式 T × r < capacity 收紧周期（见 log_rotator.py --help）")
            return 2
        return 0

    if args.cmd == "ingest-metrics":
        try:
            res = rt.ingest_metrics(path=args.file_path, label=args.label)
        except (OSError, ValueError) as e:
            print(f"[FAIL] {e}")
            return 1
        print(f"[INGEST OK] metrics 追加 1 块"
              f"（当前段 {res['current_entries']} 块 / {res['current_bytes']} 字节）")
        if res["rotated"]:
            print(f"  [ROTATE] {res['rotated_why']} → 封存 {res['sealed']}"
                  f"，现存封存段 {res['sealed_files']} 个")
        return 0

    if args.cmd == "rotate":
        sealed = rt.rotate(args.why)
        print(f"[ROTATE OK] 封存 {sealed or '（当前段为空，未封存）'}"
              f"；现存封存段 {len(rt.sealed())} 个（keep={rt.keep}）")
        return 0

    if args.cmd == "status":
        s = rt.stats()
        t0, t1 = rt.earliest_last() if args.kind == "soe" else (None, None)
        print(f"== {s['kind']} 存储状态 ==")
        print(f"  目录            : {s['dir']}")
        print(f"  封存段 / keep   : {s['sealed_files']} / {s['keep']}")
        print(f"  当前段          : {s['current_entries']} 条 / {s['current_bytes']} 字节"
              f"（阈值 {s['max_entries']} 条 / {s['max_bytes']} 字节）")
        if args.kind == "soe":
            print(f"  保留窗口        : t ∈ [{t0}, {t1}]" if t0 is not None
                  else "  保留窗口        : （空）")
        print(f"  已观测淘汰(dropped): {s['dropped_seen']}")
        if rt.last_interval_s is not None:
            print(f"  最近导出间隔    : {rt.last_interval_s} s")
        return 0

    if args.cmd == "query":
        evs = rt.query(t0=args.t0, t1=args.t1, level=args.level,
                       code=args.code, source=args.source)
        if args.as_json:
            print(json.dumps({"count": len(evs),
                              "events": [e.to_dict() for e in evs]},
                             ensure_ascii=False, indent=2))
        else:
            sys.stdout.write(SOE_HEADER + "\n")
            for e in evs:
                sys.stdout.write(e.to_csv())
            print(f"# 命中 {len(evs)} 条（扫描 {len(rt.all_segments())} 个段）",
                  file=sys.stderr)
        return 0

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
