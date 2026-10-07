#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""19/tests —— 部署脚本（.bat）与《现场作业指导书》测试。

本模块主体是**部署与运维脚本**，所以测试对象就是这些脚本本身与随附文档。
四个落点：

  [A] 脚本齐套与行尾编码 —— 6 个 .bat 存在、CRLF、过 `scripts/fix_bat_encoding.py
      --check`（该检查器同时管"块内路径变量 %CD% 展开"这类整树崩解析的坑）。
  [B] 含 `!VAR!` 的行必须纯 ASCII —— 上一轮实测：`.bat` 里"中文字符紧邻
      `!VAR!`"会把变量**静默吃掉**，退出码仍是 0。这条断言是那个坑的第一道锁。
  [C] dry-run / check **真跑一次** —— 且必须**零副作用**（跑完安装根顶层
      条目一字不差）。
  [D] 指导书的**点表核对清单**与 `13/sim/modbus_slave.py --dump-tsv` 逐点对账 ——
      文档里那张 32 点表不是抄的，是从可执行点表里比对出来的。
  [E] 文档齐套（部署形态决定 / 上电前流程 / 验收判据 / 回滚 / 安全规程措辞）。
  [F] `build_test.bat` 的汇总契约（`findstr /b "PASS="` 那一行不能歪）。

★ 为什么用**子进程**跑 .bat 而不是 import 它们：
  .bat 只能由 cmd.exe 解释；而且从 Git Bash 直接跑会被仓库路径里的括号坑到
  （`D:\\wb(cn)\\BMS`），所以统统走 `cmd /c`，cwd 固定到安装根。
★ 为什么**不**把这些 .bat 的 stdin 重定向到 NUL：
  Windows 上 `isatty(NUL)` 为真，一旦有人开了守护器的 stdin 看门狗，
  ">nul <nul" 会让它打印完 banner 就自杀（任务书点名的坑）。
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile

import harness as H
from harness import T

MODULE = H.MODULE
REPO = H.REPO
SCRIPTS = os.path.join(MODULE, "scripts")
TESTS = os.path.join(MODULE, "tests")
DOCS = os.path.join(MODULE, "docs")
DOC = os.path.join(DOCS, "现场作业指导书.md")

BATS = [
    "build_test.bat",
    "install_task.bat",
    "uninstall_task.bat",
    "install_service.bat",
    "uninstall_service.bat",
    "run_supervisor.bat",
]

# `build_test.bat` 的 `call :run <name>` 必须与 tests/ 下真实的测试文件集合一致；
# 少一个就是"漏跑"，多一个就是"指向不存在的文件"，两种都是静默的。
TEST_SUITES = ["test_config_store", "test_log_rotator", "test_supervisor", "test_scripts"]

FIX_BAT = os.path.join(REPO, "scripts", "fix_bat_encoding.py")
SIM = os.path.join(REPO, "13", "sim", "modbus_slave.py")
EXES = ["rtdb_initializer.exe", "device_side.exe", "ems_side.exe"]

# 只在**非注释行**上判：注释里写 `!VAR!` 不会被 cmd 展开，不构成那个坑。
VAR_RE = re.compile(rb"![A-Za-z_][A-Za-z0-9_]*!")
PT_BEGIN = "<!-- POINT-TABLE-BEGIN -->"
PT_END = "<!-- POINT-TABLE-END -->"


# =====================================================================
# 小工具
# =====================================================================
def bat_path(name: str) -> str:
    return os.path.join(SCRIPTS, name)


def read_bytes(path: str) -> bytes:
    with open(path, "rb") as f:
        return f.read()


def run_bat(name: str, *args: str, timeout: int = 180):
    """cmd /c 跑 .bat。返回 (rc, 合并后的文本)。stdin 不重定向（见模块头）。"""
    rel = os.path.join("19", "scripts", name)
    r = subprocess.run(["cmd", "/c", rel] + list(args), cwd=REPO,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def run_py(args, timeout: int = 180):
    r = subprocess.run([sys.executable] + list(args), cwd=REPO,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def bat_lines(raw: bytes):
    """把 CRLF 归一后切行（保留原始字节，用于 ASCII 判定）。"""
    return raw.replace(b"\r\n", b"\n").split(b"\n")


def is_comment(line: bytes) -> bool:
    s = line.strip()
    return s.lower().startswith(b"rem") or s.startswith(b"::")


def var_lines_nonascii(raw: bytes):
    """返回 [(行号, 行字节)]：非注释行、含 `!VAR!`、且含非 ASCII 字节。

    这三者同时成立 = 上一轮那个"中文紧邻 !VAR! 静默吃掉变量"的坑。
    """
    hits = []
    for i, line in enumerate(bat_lines(raw), 1):
        if is_comment(line):
            continue
        if not VAR_RE.search(line):
            continue
        if any(b > 0x7F for b in line):
            hits.append((i, line))
    return hits


def count_var_lines(raw: bytes) -> int:
    return sum(1 for i, ln in enumerate(bat_lines(raw), 1)
               if not is_comment(ln) and VAR_RE.search(ln))


def first_line_index(raw: bytes, needle: bytes):
    """第一个**含** needle 的物理行号（1 起）；找不到返回 None。"""
    for i, line in enumerate(bat_lines(raw), 1):
        if needle in line:
            return i
    return None


def first_exec_line(raw: bytes, needle: bytes):
    """第一个**以** needle 开头（忽略缩进）的行号 —— 即"真的执行这条命令"的那行。

    与 `first_line_index` 的区别很关键：`set "SCHTASKCMD=schtasks /Create ..."`
    只是拼命令串、`echo schtasks /Create ...` 只是打印，两者都**不碰系统**。
    用"含"去找会把这些当成真调用，得出错误的先后关系。
    """
    low = needle.lower()
    for i, line in enumerate(bat_lines(raw), 1):
        if line.strip().lower().startswith(low):
            return i
    return None


def bare_lf_lines(raw: bytes):
    """返回**以裸 LF 结尾**（前面不是 CR）的行号 —— 即行尾没统一成 CRLF。

    只看真正以 LF 终止的段，不碰"末行缺换行"这种情形（那不是本项要管的）。
    """
    out = []
    parts = raw.split(b"\n")
    for i, seg in enumerate(parts[:-1], 1):
        if not seg.endswith(b"\r"):
            out.append(i)
    return out


# =====================================================================
# 点表对账
# =====================================================================
def parse_md_point_table(md_text: str):
    """取 POINT-TABLE 标记之间那张 markdown 表，返回 [[#, 点名, 表, 地址, ...]]。"""
    lines = md_text.replace("\r\n", "\n").split("\n")
    if PT_BEGIN not in lines or PT_END not in lines:
        return None
    i0, i1 = lines.index(PT_BEGIN), lines.index(PT_END)
    rows = []
    for ln in lines[i0 + 1:i1]:
        s = ln.strip()
        if not s.startswith("|"):
            continue
        cells = [c.strip() for c in s.strip("|").split("|")]
        if not cells or cells[0] == "#":
            continue
        if set(cells[0]) <= set("-"):       # | --- | --- | 分隔行
            continue
        rows.append(cells)
    return rows


def parse_tsv(text: str):
    """13/ --dump-tsv 的 8 列 TSV → [[index, name, table, addr, enc, wo, scale, wr]]。"""
    rows = []
    for ln in text.replace("\r\n", "\n").split("\n"):
        if not ln.strip():
            continue
        c = ln.split("\t")
        if len(c) < 8:
            continue
        rows.append(c[:8])
    return rows


def reconcile(md_rows, tsv_rows):
    """逐点对账，返回不一致清单。正向断言期望空表；
    反向守卫喂一份被篡改的 TSV，期望非空。"""
    bad = []
    if len(md_rows) != len(tsv_rows):
        bad.append(("count", len(md_rows), len(tsv_rows)))
    for md, ts in zip(md_rows, tsv_rows):
        try:
            same = (int(md[0]) == int(ts[0]) and md[1] == ts[1]
                    and md[2] == ts[2] and int(md[3]) == int(ts[3]))
        except (ValueError, IndexError):
            same = False
        if not same:
            bad.append((md[0], md[1], ts[1], md[2], ts[2], md[3], ts[3]))
    return bad


# =====================================================================
def main() -> int:
    H.main_guard()
    t = T("19/ 部署脚本与现场作业指导书 自检")
    H.banner("19/ scripts & site playbook —— 部署脚本与文档")

    # =============================================================
    t.section("A 脚本齐套 / 行尾 / 编码检查器")
    # =============================================================
    raw = {}
    for name in BATS:
        p = bat_path(name)
        exists = os.path.isfile(p)
        t.ok(exists, f"A {name} 存在")
        if not exists:
            continue
        raw[name] = read_bytes(p)
        # 首行 @echo off：没有它，cmd 会把每条命令回显出来，中文行会被二次解析
        t.ok(bat_lines(raw[name])[0].strip().lower() == b"@echo off",
             f"A {name} 首行是 @echo off")
        # CRLF：纯 LF 的 .bat 里 `rem`/`echo` 前缀会被 cmd 吃掉
        t.eq(bare_lf_lines(raw[name]), [], f"A {name} 行尾统一 CRLF")
        # 控制字符（除 TAB/CR/LF）：python 批量改写常把 \v \f \b 写进去，错得无声
        ctrl = [(i, b) for i, ln in enumerate(bat_lines(raw[name]), 1)
                for b in ln if b < 0x20 and b not in (0x09,)]
        t.eq(ctrl, [], f"A {name} 无控制字符")

    t.eq(len(raw), len(BATS), "A 六个 .bat 全部可读（反向守卫：一个都没少读）")

    if os.path.isfile(FIX_BAT):
        rc, out = run_py([os.path.join("scripts", "fix_bat_encoding.py"),
                          "--check", os.path.join("19", "scripts")])
        t.eq(rc, 0, "A fix_bat_encoding --check 19/scripts 通过")
        t.contains(out, "全部合规", "A 检查器明确报『全部合规』")

        # 反向守卫：造一个纯 LF 的 .bat，检查器必须判红
        tmpd = tempfile.mkdtemp(prefix="ems19_batcheck_")
        try:
            badp = os.path.join(tmpd, "bad.bat")
            with open(badp, "wb") as f:
                f.write(b"@echo off\necho hi\n")
            rc2, out2 = run_py([os.path.join("scripts", "fix_bat_encoding.py"),
                                "--check", badp])
            t.eq(rc2, 1, "★ A 反向守卫：LF 行尾的 .bat 被检查器判红（rc=1）")
            t.contains(out2, "NEED FIX", "★ A 反向守卫：检查器指出具体文件")
        finally:
            shutil.rmtree(tmpd, ignore_errors=True)
    else:
        t.skip("scripts/fix_bat_encoding.py 不存在，跳过 CRLF 检查器")

    # =============================================================
    t.section("B 含 !VAR! 的行必须纯 ASCII")
    # =============================================================
    total_var_lines = 0
    for name in BATS:
        if name not in raw:
            continue
        n = count_var_lines(raw[name])
        total_var_lines += n
        hits = var_lines_nonascii(raw[name])
        t.eq(hits, [], f"B {name} 含 !VAR! 的行全 ASCII（命中 {n} 行）")
    t.gt(total_var_lines, 0,
         "★ B 反向守卫：确实扫到了含 !VAR! 的行（不是空扫）")

    # 反向守卫：同一段检测逻辑，喂一行"中文 + !VAR!"必须命中
    probe = "echo 安装根 !ROOT! 就位".encode("utf-8")
    t.eq(len(var_lines_nonascii(b"%s\r\n" % probe)), 1,
         "★ B 反向守卫：『中文紧邻 !VAR!』被检测逻辑抓出来")
    t.eq(len(var_lines_nonascii(b"echo root !ROOT! ready\r\n")), 0,
         "★ B 反向守卫：同样的行去掉中文就不报警（不是全盘误报）")

    # =============================================================
    t.section("C install / uninstall / supervisor 脚本真跑（零副作用）")
    # =============================================================
    before = sorted(os.listdir(REPO))
    have_exes = all(os.path.isfile(os.path.join(REPO, "11", "build", e)) for e in EXES)
    t.guard(have_exes, "C 11/build 三件产物齐（install_task --check 的前置）")

    if have_exes:
        rc, out = run_bat("install_task.bat", "--dry-run")
        t.eq(rc, 0, "C install_task --dry-run 退出码 0")
        t.contains(out, "[DRY-RUN]", "C dry-run 明确标注 [DRY-RUN]")
        t.contains(out, "nothing was changed", "C dry-run 声明未改任何东西")
        t.ok("scheduled task registered" not in out,
             "★ C dry-run 没有真的注册任务")

        rc, out = run_bat("install_task.bat", "--check")
        t.eq(rc, 0, "C install_task --check 退出码 0")
        t.contains(out, "[OK] check only", "C check 只做前置检查")
        t.contains(out, "python", "C check 报了找到的 python 解释器")
    else:
        t.skip("11/build/exe 缺失 → install_task.bat --check/--dry-run 跳过")

    rc, out = run_bat("uninstall_task.bat", "--dry-run")
    t.eq(rc, 0, "C uninstall_task --dry-run 退出码 0")
    t.contains(out, "[DRY-RUN]", "C 卸载 dry-run 明确标注")
    t.contains(out, "nothing was changed", "C 卸载 dry-run 声明未改任何东西")
    t.ok("scheduled task removed" not in out,
         "★ C 卸载 dry-run 没有真的删任务")

    # install_service.bat 故意拒绝在没有 SCM 包装器时装成服务，退出码 2
    rc, out = run_bat("install_service.bat", "--recipe")
    t.eq(rc, 2, "C install_service --recipe 退出码 2（= 未安装，非失败）")
    t.contains(out, "NOT INSTALLED", "C 服务路线明确报『未安装』")
    t.contains(out, "SERVICE_RUNNING", "C 讲清 SCM 握手（不握手 ~30 s 会被杀）")
    t.contains(out, "install_task.bat", "C 指回计划任务这条已落地路线")

    rc, out = run_bat("run_supervisor.bat", "plan")
    t.eq(rc, 0, "C run_supervisor plan 退出码 0")
    t.contains(out, "rtdb_initializer", "C plan 列出初始化器")
    t.contains(out, "device_side", "C plan 列出设备侧")
    t.contains(out, "ems_side", "C plan 列出 EMS 侧")
    try:
        i1 = out.index("1. rtdb_initializer")
        i2 = out.index("2. device_side")
        i3 = out.index("3. ems_side")
        t.ok(i1 < i2 < i3, "★ C plan 的启动顺序 = 初始化器→设备侧→EMS")
    except ValueError:
        t.ok(False, "★ C plan 的启动顺序可解析（编号 1./2./3. 都在）")

    rc, out = run_bat("run_supervisor.bat", "check")
    t.eq(rc, 0, "C run_supervisor check 退出码 0")
    t.contains(out, "[CHECK OK]", "C check 报前置检查通过")

    after = sorted(os.listdir(REPO))
    t.eq(after, before, "★ C 跑完这一整段，安装根顶层条目一字不差（零副作用）")
    t.ok(not os.path.exists(os.path.join(REPO, "conf", "supervisor.json")),
         "★ C 没有偷偷生成 conf/supervisor.json")
    t.ok(not os.path.exists(os.path.join(REPO, "log", "supervisor.log")),
         "★ C 没有偷偷生成 log/supervisor.log")

    # 静态不变量：dry-run / check 必须在任何写盘动作**之前**返回
    if "install_task.bat" in raw:
        b = raw["install_task.bat"]
        i_check = first_line_index(b, b"goto check_only")
        i_dry = first_line_index(b, b"goto dry_run_task")
        i_mkdir = first_line_index(b, b"mkdir")
        t.ok(i_check is not None and i_dry is not None and i_mkdir is not None,
             "C install_task 里 check_only/dry_run/mkdir 三处锚点都在")
        if None not in (i_check, i_dry, i_mkdir):
            t.ok(i_check < i_mkdir,
                 "★ C --check 在任何 mkdir 之前返回")
            t.ok(i_dry < i_mkdir,
                 "★ C --dry-run 在任何 mkdir 之前返回")
        i_schtask = first_exec_line(b, b"schtasks /Create")
        t.ok(i_schtask is not None and i_dry < i_schtask,
             "★ C --dry-run 在真正执行 schtasks /Create 之前返回")

    if "uninstall_task.bat" in raw:
        b = raw["uninstall_task.bat"]
        i_dry = first_line_index(b, b"goto dry_run_del")
        i_sch = first_exec_line(b, b"schtasks /Query")
        t.ok(i_dry is not None and i_sch is not None and i_dry < i_sch,
             "★ C 卸载 --dry-run 在任何 schtasks 调用之前返回")

    # =============================================================
    t.section("D 点表核对清单 × 13/ --dump-tsv 逐点对账")
    # =============================================================
    if not os.path.isfile(DOC):
        t.skip("docs/现场作业指导书.md 不存在 → 点表对账跳过")
    else:
        md = H.read_text(DOC)
        md_rows = parse_md_point_table(md)
        t.ok(md_rows is not None, "D 指导书里找得到 POINT-TABLE 标记区")
        if md_rows is not None:
            t.eq(len(md_rows), 32, "D 指导书点表 32 行")
            if os.path.isfile(SIM):
                rc, out = run_py([os.path.join("13", "sim", "modbus_slave.py"),
                                  "--dump-tsv"])
                t.eq(rc, 0, "D 13/sim/modbus_slave.py --dump-tsv 退出码 0")
                tsv = parse_tsv(out)
                t.eq(len(tsv), 32, "D --dump-tsv 32 行")
                if len(tsv) == 32:
                    t.eq(reconcile(md_rows, tsv), [],
                         "★ D 32 点逐点对账（序号/点名/表/地址）零差异")
                    for k, (m, s) in enumerate(zip(md_rows, tsv)):
                        t.eq(int(m[0]), int(s[0]), f"D[{k}] 序号一致")
                        t.ok((m[1], m[2], int(m[3])) == (s[1], s[2], int(s[3])),
                             f"D[{k}] {m[1]} = {s[2]}[{s[3]}] 与可执行点表一致")

                    # 可写点只应有三条 CMD.*，且都在 HR 表
                    wr = [(s[1], s[2]) for s in tsv if s[7] == "1"]
                    t.eq(sorted(n for n, _ in wr),
                         ["CMD.P_BAT", "CMD.P_LOWER", "CMD.P_UPPER"],
                         "D 可写点恰为三条 CMD.*")
                    t.ok(all(tb == "HR" for _, tb in wr),
                         "D 可写点全在 HR（保持寄存器）表")

                    # 字序：只有 MEAS.P_BAT 是 BA（低字在前）—— 文档写死了这条
                    ba = [s[1] for s in tsv if s[5] == "BA"]
                    t.eq(ba, ["MEAS.P_BAT"], "D 低字在前（BA）只有 MEAS.P_BAT 一个")
                    t.contains(md, "低字在前", "D 指导书写明了 MEAS.P_BAT 字序特殊")

                    # DI 状态位 8 条
                    t.eq(sum(1 for s in tsv if s[2] == "DI"), 8, "D DI 状态位 8 条")

                    # 定点换算与文档说明一致（u16/i16 的 ×10000 / ×10）
                    sc = {s[1]: s[6] for s in tsv}
                    t.eq(sc.get("MEAS.SOC"), "10000", "D MEAS.SOC 定点 ×10000")
                    t.eq(sc.get("MEAS.SOH"), "10000", "D MEAS.SOH 定点 ×10000")
                    t.eq(sc.get("MEAS.T_C"), "10", "D MEAS.T_C 定点 ×10")
                    t.contains(md, "×10000", "D 指导书写明 ×10000")
                    t.contains(md, "×10", "D 指导书写明 ×10")

                    # 反向守卫：篡改一行点名，对账必须报出来
                    tsv_bad = [list(r) for r in tsv]
                    tsv_bad[2][1] = "MEAS.P_BATTERY"
                    bad = reconcile(md_rows, tsv_bad)
                    t.eq(len(bad), 1,
                         "★ D 反向守卫：改掉一个点名后对账恰好报 1 处")
                    t.ok(bad and bad[0][1] == "MEAS.P_BAT",
                         "★ D 反向守卫：报出的正是被改的那一点")
            else:
                t.skip("13/sim/modbus_slave.py 不存在 → 点表对账跳过")

    # =============================================================
    t.section("E 指导书齐套")
    # =============================================================
    if not os.path.isfile(DOC):
        t.skip("docs/现场作业指导书.md 不存在")
    else:
        md = H.read_text(DOC)
        for key in ("前置条件", "部署形态", "点表核对清单", "上电前流程固化",
                    "验收判据", "回滚", "换人", "应急处置"):
            t.contains(md, key, f"E 指导书含小节『{key}』")
        # 命令原文：每一步都得能照着敲
        for cmd in ("--dump-template", "--check", "--capture", "modbus_probe",
                    "install_task.bat", "--dump-tsv"):
            t.contains(md, cmd, f"E 指导书给出命令原文 `{cmd}`")
        # 安全规程措辞必须显式写在文档里（不是缺陷，是纪律）
        t.contains(md, "不自动带载", "★ E 写明『故障后不自动带载』是安全规程")
        # 活文件那一步必须写出来：不落 conf\ems_config.json，rollback 就没有
        # "当前"可备份，§6.2 的判据④（版本数 +1）**验不到**（实测过）。
        t.contains(md, "--capture conf\\ems_config.json",
                   "★ E 写明『落活文件』这一步（否则回滚判据④验不到）")
        t.contains(md, "活文件",
                   "★ E 写清 conf\\ems_config.json 是活文件（区别于纸面配置）")
        t.contains(md, "回滚前自动备份为 None",
                   "★ E 把『没有活文件』的失败现象也写进排查表")
        # 文档引用到的 19/ 脚本必须真实存在（防文档指向空气）
        for name in ("install_task.bat", "uninstall_task.bat", "run_supervisor.bat"):
            t.ok(os.path.isfile(bat_path(name)),
                 f"E 文档引用的 {name} 真实存在")
        # 计划任务 vs 服务的取舍必须落纸
        t.contains(md, "计划任务", "E 写清了选计划任务")
        t.contains(md, "SCM", "E 写清了不选服务的理由（SCM 握手）")

    # =============================================================
    t.section("F build_test.bat 汇总契约")
    # =============================================================
    if "build_test.bat" not in raw:
        t.skip("build_test.bat 缺失")
    else:
        bt = raw["build_test.bat"]
        btxt = bt.decode("utf-8", "replace")
        t.contains(btxt, 'findstr /b "PASS="',
                   "★ F 汇总靠 findstr /b \"PASS=\" 抓固定行（不能靠人眼读中文）")
        # 每个测试套件都真的被 call 到，且与 tests/ 下实际测试文件一致
        called = set(re.findall(r"call :run\s+(\S+)", btxt))
        on_disk = {f[:-3] for f in os.listdir(TESTS)
                   if f.startswith("test_") and f.endswith(".py")}
        t.eq(sorted(called), sorted(on_disk),
             "★ F call :run 的集合 == tests/ 下 test_*.py 集合（不重不漏）")
        t.eq(sorted(on_disk), sorted(TEST_SUITES),
             "F 测试套件清单与预期一致")
        # 汇总数字所在行必须纯 ASCII（中文 echo 行后面的数字会被吞）
        for ln in bat_lines(bt):
            if b"assertions total:" in ln:
                t.ok(all(b < 0x80 for b in ln),
                     "★ F 汇总行 assertions total 是纯 ASCII")
                t.ok(b"PASS=!TOTAL!" in ln,
                     "★ F 汇总行的 PASS= 后直接跟变量（不含中文）")
        # 纯 ASCII 状态文件落盘（供上层脚本判定）
        t.contains(btxt, "test_status.txt", "F 状态行落盘 build\\test_status.txt")

    return t.report()


if __name__ == "__main__":
    raise SystemExit(main())
