#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""19/ 配置落盘与版本 —— 现场配置的"存一份 / 比一比 / 退回去"。

背景（`docs/交接/下一步工作交接说明.md` §3.3 第 2 条）
------------------------------------------------------
P1 已经把"现场部署只改配置"做完（89 字段 + `--check` 语法/语义/装配校验），
并且给了 `--capture`：把**运行中实际生效的值**导出成 JSON。交接说明原话是

    P1 的 `--capture` 导出的「实际生效值」就是现成的回滚点与交接文档。

这一句在"人工用一次"时成立，但现场要的是"**每次调参自动留一版、出事能退**"。
本文件补的就是这件事：把 `--capture` 的产物落成**带时间戳的版本**，并支持
`list` / `diff` / `rollback`。

落盘约定
--------
```
<安装根>/
  conf/
    ems_config.json             ← 当前生效配置（唯一的"活"文件）
    history/
      v0001_20260926-120000.json
      v0002_20260926-143500.json
      index.json                ← 各版本的备注（**advisory，不是真相源**）
```

三条设计决策与"为什么"
----------------------
① **版本号与时间戳编进文件名**，不建数据库。
   现场服务器上多一个 SQLite 就多一样要备份、要修、会锁的东西；
   而"文件在不在"本身就是最抗损坏的索引 —— 一个坏掉的 index.json
   不该让运维连"有哪几版配置"都问不出来。
   因此 `index.json` **只存备注**，`list` 的真实数据全部来自目录扫描。
   判跑法：删掉 `index.json`，`list` 依然给出全部版本（只是备注为空）。

② **`rollback` 自己也是可回滚的。** 回滚前**无条件**先把当前生效配置存一版。
   理由：回滚是一次不可逆的写操作 —— 如果它覆盖掉的正好是"唯一一份还在起作用的
   现场调参结果"，那这次回滚就把现场最强的证据删了。
   所以"回滚后 `list` 的版本数 +1"是**硬判据**，不是副产品。

③ **写文件一律 tmp + os.replace 原子替换。** `conf/ems_config.json` 是
   进程启动时读的那一份。写到一半断电 = 现场下次启动读到一个截断的 JSON，
   而 P1 的行为是"配置错误就拒绝启动" —— 等于**一个写盘事故变成一次停机**。
   `os.replace` 在同一卷上是原子的，最坏情况是"这次回滚没生效"，不是"配置坏了"。

与 P1 的边界
------------
本文件**不解析、不校验**配置语义 —— 那是 P1 `validate()` 的职责（它本来就写在
`--check` 里）。这里只做"字节级的搬运 + 结构级的比对"，因此不需要跟着 89 个字段
一起改。**唯一的例外**是 `diff` 需要递归结构：它对字段名一无所知，只按 JSON
的结构走，所以 P1 加字段时这里不用动（有测试钉住这一点）。

命令
----
    python config_store.py --root <root> snapshot [--note T] [--from FILE]
    python config_store.py --root <root> list
    python config_store.py --root <root> show <ver>
    python config_store.py --root <root> diff <a> <b>
    python config_store.py --root <root> rollback <ver>
    python config_store.py --root <root> verify <ver>

`<ver>` 可以是 `latest` / `active` / `v0007` / `7`。
退出码：0 正常 / 1 用法或 IO 错误 / 2 校验不通过。
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys

# 19/src/ -> 19/ -> <repo>
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

ACTIVE_REL = os.path.join("conf", "ems_config.json")
HISTORY_REL = os.path.join("conf", "history")

# v0007_20260926-143500.json  —— 版本号 4 位起（够 9999 次现场调参；不够就自然变宽）
HIST_RE = re.compile(r"^v(\d+)_(\d{8}-\d{6})$")
STAMP_FMT = "%Y%m%d-%H%M%S"


# =====================================================================
# 纯函数：结构化比对（对字段名一无所知，只按 JSON 结构走）
# =====================================================================
def canonical(obj) -> str:
    """规范化序列化：键排序 + 2 空格缩进 + 结尾换行。

    ★ 为什么要 canonical 而不是"原样拷贝文本"：
      `diff` 与"往返读回逐字段一致"都要求**比较的是值，不是排版**。
      原样拷贝会把"换行是 LF 还是 CRLF""缩进是 2 还是 4"变成版本差异，
      让真正要看的那条字段变化淹在噪声里。
    """
    return json.dumps(obj, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def load_json(path: str):
    with open(path, "r", encoding="utf-8-sig") as f:
        return json.load(f)


def flatten(node, prefix: str = "", out: dict | None = None) -> dict:
    """把嵌套 JSON 摊平成 {叶子路径: 值}。

    路径形如 `safety.soc_min` / `strategies.S04_DEMAND_MGMT.params.Kp` / `xs[0]`。
    叶子 = 标量（含 null）。空 dict / 空 list 本身算一个叶子（否则"整节被删"
    会变成零条变化，看起来像没改）。
    """
    if out is None:
        out = {}
    if isinstance(node, dict):
        if not node:
            out[prefix or "$"] = {}
            return out
        for k, v in node.items():
            flatten(v, f"{prefix}.{k}" if prefix else str(k), out)
    elif isinstance(node, list):
        if not node:
            out[prefix or "$"] = []
            return out
        for i, v in enumerate(node):
            flatten(v, f"{prefix}[{i}]", out)
    else:
        out[prefix or "$"] = node
    return out


def diff_objects(a, b) -> list[tuple[str, str, object, object]]:
    """逐字段比较，返回 [(路径, kind, 旧值, 新值)]，kind ∈ changed/added/removed。

    ★ 类型变了怎么办（如 `limits` 从对象变成一个数）：摊平后叶子路径
      从 `limits.d_target_kw` 变成 `limits`，于是同时出现一条 removed
      与一条 added —— 这是**故意的**：它读起来就是"整个 limits 被换掉了"，
      比硬造一个 `type_changed` 更好懂，也不需要 diff 知道字段语义。
    """
    fa, fb = flatten(a), flatten(b)
    changes: list[tuple[str, str, object, object]] = []
    for key in sorted(set(fa) | set(fb)):
        in_a, in_b = key in fa, key in fb
        if in_a and in_b:
            if fa[key] != fb[key]:
                changes.append((key, "changed", fa[key], fb[key]))
        elif in_b:
            changes.append((key, "added", None, fb[key]))
        else:
            changes.append((key, "removed", fa[key], None))
    return changes


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def _fmt(v) -> str:
    if v is None:
        return "null"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return repr(v)
    return str(v)


def default_capture_exe() -> str | None:
    """定位 P1 的 `ems_config.exe`。

    顺序：`EMS_CONFIG_EXE` 环境变量 → `P1/build/ems_config.exe`。
    找不到时返回 None —— 由调用方决定是 SKIP 还是报错（`snapshot --from` 不需要它）。
    """
    env = os.environ.get("EMS_CONFIG_EXE", "").strip()
    if env:
        return env
    cand = os.path.join(REPO, "P1", "build", "ems_config.exe")
    return cand if os.path.isfile(cand) else None


# =====================================================================
# 版本库
# =====================================================================
class Version:
    __slots__ = ("seq", "stamp", "path", "name")

    def __init__(self, seq: int, stamp: str, path: str) -> None:
        self.seq = seq
        self.stamp = stamp
        self.path = path
        self.name = os.path.splitext(os.path.basename(path))[0]

    def __repr__(self) -> str:  # pragma: no cover - 调试用
        return f"<Version {self.name}>"


class ConfigStore:
    def __init__(self, root: str,
                 active_rel: str = ACTIVE_REL,
                 history_rel: str = HISTORY_REL) -> None:
        self.root = os.path.abspath(root)
        self.active = os.path.join(self.root, active_rel)
        self.history = os.path.join(self.root, history_rel)
        self.index_path = os.path.join(self.history, "index.json")

    # ---- 目录 ----

    def ensure_dirs(self) -> None:
        os.makedirs(self.history, exist_ok=True)

    # ---- 版本发现（目录是真相源） ----

    def versions(self) -> list[Version]:
        if not os.path.isdir(self.history):
            return []
        out: list[Version] = []
        for name in os.listdir(self.history):
            stem, ext = os.path.splitext(name)
            if ext.lower() != ".json":
                continue
            m = HIST_RE.match(stem)
            if not m:
                continue
            out.append(Version(int(m.group(1)), m.group(2),
                               os.path.join(self.history, name)))
        out.sort(key=lambda v: (v.seq, v.stamp))
        return out

    def next_seq(self) -> int:
        vs = self.versions()
        return (vs[-1].seq + 1) if vs else 1

    def resolve(self, ref: str) -> Version:
        """`latest` / `active` / `v0007` / `7` → Version。"""
        ref = (ref or "").strip()
        vs = self.versions()
        if ref in ("latest", "last") or ref == "":
            if not vs:
                raise KeyError("没有任何版本（先跑 snapshot）")
            return vs[-1]
        if ref == "active":
            if not os.path.isfile(self.active):
                raise KeyError(f"当前生效配置不存在：{self.active}")
            # active 的序号 = 它的内容等于哪一版；不等则报错（让人显式选版本）
            cur = load_json(self.active)
            for v in reversed(vs):
                if load_json(v.path) == cur:
                    return v
            raise KeyError("当前生效配置与 history/ 里任何一版都不一致，请显式指定版本号")
        m = re.match(r"^v?(\d+)$", ref)
        if not m:
            # 也接受 `list` 直接打印出来的全名（便于复制粘贴）
            for v in vs:
                if v.name == ref:
                    return v
            raise KeyError(f"无法解析版本引用：{ref!r}"
                           f"（可用 latest / active / v0007 / 7 / v0007_20260926-120000）")
        seq = int(m.group(1))
        for v in vs:
            if v.seq == seq:
                return v
        raise KeyError(f"版本 {ref!r} 不存在（现有 {len(vs)} 版）")

    # ---- 备注索引（advisory） ----

    def _read_index(self) -> dict:
        if not os.path.isfile(self.index_path):
            return {}
        try:
            d = load_json(self.index_path)
            return d if isinstance(d, dict) else {}
        except (ValueError, OSError):
            # 索引坏了不该让 list/diff/rollback 全挂 —— 它只是备注
            return {}

    def _write_index(self, idx: dict) -> None:
        self._atomic_write(self.index_path, canonical(idx))

    def _atomic_write(self, path: str, text: str) -> None:
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)

    def _stamp(self) -> str:
        return datetime.datetime.now().strftime(STAMP_FMT)

    # ---- 写入 ----

    def snapshot(self, source: str | None = None, note: str = "",
                 capture_exe: str | None = None,
                 now: str | None = None) -> Version:
        """存一版。

        source 给了 → 直接用它（必须是 JSON 对象）；
        没给 → 调 `ems_config.exe --capture <tmp>` 取**实际生效值**。

        ★ `now` 只给测试用（注入时间戳以免同一秒内两版撞名）。
        """
        if source is not None:
            obj = load_json(source)
        else:
            obj = self.capture(capture_exe)
        if not isinstance(obj, dict):
            raise ValueError("配置必须是一个 JSON 对象")

        self.ensure_dirs()
        seq = self.next_seq()
        stamp = now or self._stamp()
        name = f"v{seq:04d}_{stamp}"
        target = os.path.join(self.history, name + ".json")
        # 同一秒两版：seq 不同，名字不会撞（seq 是主键）
        self._atomic_write(target, canonical(obj))

        idx = self._read_index()
        idx[name] = {
            "note": note,
            "created": stamp,
            "sha256": sha256_text(canonical(obj)),
            "fields": len(flatten(obj)),
            "origin": "capture" if source is None else os.path.abspath(source),
        }
        self._write_index(idx)
        return Version(seq, stamp, target)

    def capture(self, capture_exe: str | None = None) -> dict:
        """调 P1 的 `--capture` 取运行中实际生效值。"""
        exe = capture_exe or default_capture_exe()
        if not exe:
            raise FileNotFoundError(
                "找不到 ems_config.exe：设 EMS_CONFIG_EXE 或先跑 P1/scripts/build.bat；"
                "也可以改用 `snapshot --from <captured.json>`")
        if not os.path.isfile(exe):
            raise FileNotFoundError(f"ems_config.exe 不存在：{exe}")
        self.ensure_dirs()
        tmp = os.path.join(self.history, "_capture.tmp.json")
        if os.path.isfile(tmp):
            os.remove(tmp)
        proc = subprocess.run([exe, "--capture", tmp],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode != 0 or not os.path.isfile(tmp):
            out = (proc.stdout or b"").decode("utf-8", "replace")
            raise RuntimeError(f"[--capture 失败] rc={proc.returncode}\n{out}")
        obj = load_json(tmp)
        os.remove(tmp)
        return obj

    def save_active(self, obj) -> None:
        self._atomic_write(self.active, canonical(obj))

    def rollback(self, ref: str) -> dict:
        """回滚到某版。

        顺序是**硬要求**：先备份当前 → 再覆盖。反过来的话，一旦覆盖后写失败
        （磁盘满 / 权限），当前那份就永远没了。
        """
        target = self.resolve(ref)
        want = load_json(target.path)

        backup = None
        if os.path.isfile(self.active):
            backup = self.snapshot(source=self.active,
                                   note=f"rollback 前自动备份（目标 {target.name}）")
        self.save_active(want)
        return {
            "restored_from": target.name,
            "backup_version": backup.name if backup else None,
            "active": self.active,
        }

    # ---- 读取 ----

    def list_versions(self) -> list[dict]:
        idx = self._read_index()
        rows = []
        active_obj = load_json(self.active) if os.path.isfile(self.active) else None
        for v in self.versions():
            obj = load_json(v.path)
            meta = idx.get(v.name, {}) if isinstance(idx.get(v.name), dict) else {}
            rows.append({
                "version": v.name,
                "seq": v.seq,
                "created": v.stamp,
                "note": meta.get("note", ""),
                "sha256": meta.get("sha256") or sha256_text(canonical(obj)),
                "fields": len(flatten(obj)),
                "is_active": active_obj is not None and obj == active_obj,
                "bytes": os.path.getsize(v.path),
            })
        return rows

    def diff(self, ref_a: str, ref_b: str) -> list[tuple[str, str, object, object]]:
        a = load_json(self.resolve(ref_a).path)
        b = load_json(self.resolve(ref_b).path)
        return diff_objects(a, b)

    def verify(self, ref: str) -> list[str]:
        """往返自检：文件 → 对象 → 规范化 → 再解析，逐字段一致。"""
        v = self.resolve(ref)
        problems: list[str] = []
        text = open(v.path, "r", encoding="utf-8", newline="").read()
        obj1 = json.loads(text)
        text2 = canonical(obj1)
        obj2 = json.loads(text2)
        if flatten(obj1) != flatten(obj2):
            problems.append("往返后逐字段不一致")
        if canonical(obj1) != canonical(obj2):
            problems.append("往返后规范化文本不一致")
        # 写出去的必须已经是规范化形式（否则"文本即版本"这句话不成立）
        if text != canonical(obj1):
            problems.append("落盘文本不是规范化形式（可能被手工改过排版）")
        return problems


# =====================================================================
# CLI
# =====================================================================
def _backend(args) -> ConfigStore:
    return ConfigStore(args.root, args.active, args.history)


def _print_versions(rows: list[dict]) -> None:
    if not rows:
        print("  （没有版本。先跑：snapshot --from <captured.json>）")
        return
    print(f"  {'版本':<26} {'字段':>5} {'字节':>7}  {'激活':<4} 备注")
    for r in rows:
        mark = "*" if r["is_active"] else ""
        note = r["note"] or ""
        if len(note) > 34:
            note = note[:33] + "…"
        print(f"  {r['version']:<26} {r['fields']:>5} {r['bytes']:>7}  {mark:<4} {note}")
    print(f"  共 {len(rows)} 版；`*` = 与当前 conf/ems_config.json 内容一致")


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="19/ 配置落盘与版本")
    ap.add_argument("--root", required=True, help="安装根目录")
    ap.add_argument("--active", default=ACTIVE_REL, help="当前生效配置的相对路径")
    ap.add_argument("--history", default=HISTORY_REL, help="版本目录的相对路径")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_snap = sub.add_parser("snapshot", help="存一版（默认调 --capture）")
    p_snap.add_argument("--note", default="")
    p_snap.add_argument("--from", dest="src", default=None,
                        help="直接用一个 JSON 文件（不调 ems_config.exe）")

    sub.add_parser("list", help="列版本")

    p_show = sub.add_parser("show", help="打印某版内容")
    p_show.add_argument("version")

    p_diff = sub.add_parser("diff", help="逐字段比较两版")
    p_diff.add_argument("a")
    p_diff.add_argument("b")

    p_rb = sub.add_parser("rollback", help="回滚（回滚前自动备份当前）")
    p_rb.add_argument("version")

    p_vf = sub.add_parser("verify", help="往返自检")
    p_vf.add_argument("version", nargs="?", default="latest")

    args = ap.parse_args(argv)
    st = _backend(args)

    try:
        if args.cmd == "snapshot":
            v = st.snapshot(source=args.src, note=args.note)
            print(f"[SNAPSHOT OK] {v.name} → {os.path.relpath(v.path, st.root)}")
            return 0

        if args.cmd == "list":
            _print_versions(st.list_versions())
            return 0

        if args.cmd == "show":
            v = st.resolve(args.version)
            sys.stdout.write(canonical(load_json(v.path)))
            return 0

        if args.cmd == "diff":
            changes = st.diff(args.a, args.b)
            va, vb = st.resolve(args.a).name, st.resolve(args.b).name
            print(f"diff {va} → {vb}：{len(changes)} 处差异")
            for path, kind, old, new in changes:
                if kind == "changed":
                    print(f"  ~ {path}: {_fmt(old)} → {_fmt(new)}")
                elif kind == "added":
                    print(f"  + {path}: {_fmt(new)}")
                else:
                    print(f"  - {path}: {_fmt(old)}")
            return 0

        if args.cmd == "rollback":
            res = st.rollback(args.version)
            print(f"[ROLLBACK OK] 已回滚到 {res['restored_from']}"
                  f"（回滚前自动备份为 {res['backup_version']}）")
            print(f"  当前生效配置：{res['active']}")
            return 0

        if args.cmd == "verify":
            problems = st.verify(args.version)
            if problems:
                print(f"[VERIFY FAIL] {len(problems)} 项：")
                for p in problems:
                    print("  -", p)
                return 2
            print(f"[VERIFY OK] 往返逐字段一致（{st.resolve(args.version).name}）")
            return 0
    except (KeyError, FileNotFoundError, ValueError, RuntimeError, OSError) as e:
        print(f"[FAIL] {e}")
        return 1

    return 1


def _selfcheck() -> int:
    """`--selfcheck`：不碰文件系统的纯函数自检（给测试之外的快速排查用）。"""
    a = {"s": {"x": 1}, "t": [1, 2], "e": {}}
    b = {"s": {"x": 2}, "t": [1, 2], "e": {}, "n": True}
    ch = diff_objects(a, b)
    kinds = sorted(k for _, k, _, _ in ch)
    print(f"diff kinds={kinds} n={len(ch)}")
    return 0 if kinds == ["added", "changed"] else 1


if __name__ == "__main__":
    if "--selfcheck" in sys.argv:
        raise SystemExit(_selfcheck())
    raise SystemExit(main())
