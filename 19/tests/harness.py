"""19/ 测试脚手架 —— 项目自带断言风格（不用 pytest / unittest）。

照 `14/tests/selftest.py` 的写法收敛成一份，供本模块四个测试共用：

  · 断言式、有计数、内容缺失时**显式 SKIP**（约定 §4.5）；
  · 收尾行固定为 `PASS=n FAIL=m SKIPPED=k`（**单个空格分隔、行首就是 PASS=**）——
    `19/scripts/build_test.bat` 用 `findstr /b "PASS="` 抓这一行做累加，
    多一个前导空格就抓不到，全量汇总会静默变成 0。这一点是刻意的：
    汇总数字必须来自**被机器解析的固定行**，不是人眼从中文输出里读出来的。

★ 为什么不用 unittest：本项目的断言必须在**每一层**都印同一行格式，
  这样 C++ 侧（`EXPECT_*` 印 `PASS=%d FAIL=%d`）与 Python 侧才能在同一个
  `build_test.bat` 里被同一套 `findstr` 逻辑汇总。unittest 的收尾格式不可控。

★ 反向守卫（约定 §3.2）：`guard()` 专供"硬判据之前先证明测试确实测到了东西"。
"""

from __future__ import annotations

import os
import sys

# 让测试能 `import config_store` / `import log_rotator` / `import ems_supervisor`
HERE = os.path.dirname(os.path.abspath(__file__))
MODULE = os.path.dirname(HERE)
SRC = os.path.join(MODULE, "src")
if SRC not in sys.path:
    sys.path.insert(0, SRC)

# 仓库根（19/ 的上一级）；供测试定位 11/ P1/ 13/ 等只读依赖
REPO = os.path.dirname(MODULE)


class T:
    """最小断言收集器。"""

    def __init__(self, title: str) -> None:
        self.title = title
        self.n_pass = 0
        self.n_fail = 0
        self.n_skip = 0
        self.failures: list[str] = []
        self._section = ""

    # ---- 断言 ----

    def ok(self, cond, name: str) -> bool:
        if cond:
            self.n_pass += 1
        else:
            self.n_fail += 1
            where = f"{self._section} / {name}" if self._section else name
            self.failures.append(where)
            print(f"  [FAIL] {where}")
        return bool(cond)

    def eq(self, got, want, name: str) -> bool:
        return self.ok(got == want, f"{name}（得到 {got!r}，期望 {want!r}）")

    def ne(self, got, bad, name: str) -> bool:
        return self.ok(got != bad, f"{name}（不该等于 {bad!r}）")

    def close(self, got, want, name: str, tol: float = 1e-9) -> bool:
        try:
            good = abs(float(got) - float(want)) <= tol
        except (TypeError, ValueError):
            good = False
        return self.ok(good, f"{name}（得到 {got!r}，期望 {want!r}±{tol}）")

    def gt(self, a, b, name: str) -> bool:
        return self.ok(a > b, f"{name}（{a!r} 应 > {b!r}）")

    def ge(self, a, b, name: str) -> bool:
        return self.ok(a >= b, f"{name}（{a!r} 应 >= {b!r}）")

    def lt(self, a, b, name: str) -> bool:
        return self.ok(a < b, f"{name}（{a!r} 应 < {b!r}）")

    def contains(self, hay, needle, name: str) -> bool:
        return self.ok(needle in hay, f"{name}（{needle!r} 不在输出里）")

    def guard(self, cond, name: str) -> bool:
        """反向守卫：证明"测试确实测到了东西"。失败同样是 FAIL。"""
        return self.ok(cond, f"[反向守卫] {name}")

    def skip(self, why: str) -> None:
        self.n_skip += 1
        print(f"  [SKIP] {why}")

    # ---- 组织 ----

    def section(self, title: str) -> None:
        self._section = title
        print(f"\n[{title}]")

    def report(self) -> int:
        print()
        print("=" * 64)
        print(f" {self.title}")
        # ★ 这一行的格式是**契约**：行首 PASS=、单个空格分隔。别动。
        print(f"PASS={self.n_pass} FAIL={self.n_fail} SKIPPED={self.n_skip}")
        if self.failures:
            print(" 失败项：")
            for f in self.failures[:25]:
                print("   -", f)
            if len(self.failures) > 25:
                print(f"   ... 另有 {len(self.failures) - 25} 条")
        print("=" * 64)
        return 0 if self.n_fail == 0 else 1


def banner(title: str) -> None:
    import datetime
    print("=" * 64)
    print(f" {title}")
    print(f" 时间 {datetime.datetime.now():%Y-%m-%d %H:%M:%S}")
    print("=" * 64)


def read_text(path: str, encoding: str = "utf-8") -> str:
    with open(path, "r", encoding=encoding, newline="") as f:
        return f.read()


def main_guard():
    """统一把 stdout 设成 replace 模式：本项目 .bat 是混合编码，别让一个
    非法字节把整份测试输出打断（那会连 PASS= 收尾行一起丢掉）。"""
    try:
        sys.stdout.reconfigure(errors="replace")  # type: ignore[union-attr]
    except (AttributeError, ValueError):
        pass
