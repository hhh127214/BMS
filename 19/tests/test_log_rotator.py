#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""19/tests —— log_rotator（SOE/metrics 落盘与轮转）测试。

四条判据（任务书原文）与本文的落点：
  ① 超过大小阈值触发轮转            → [B]
  ② 保留份数正确（多余的被删最旧）    → [C]
  ③ 不丢事件（确定性事件流逐条可比）  → [D]
  ④ 轮转后仍能按时间范围查到旧事件    → [E]

另外两条本模块**特有**的：
  · 导出周期算式（`T × r < capacity`）要被钉住 → [H]（含边界两侧的反向守卫）
  · `summary.dropped` 非 0 必须报出来（"接漏"） → [F]

★ 判据有效性（约定 §3.1）：[D]/[E] 都配了"只扫当前段"的错误实现做对照 ——
  没有它，"不丢事件"可能只是"事件本来就没跨段"。
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

import harness as H
from harness import T

import log_rotator as LR

P2_SOE_H = os.path.join(H.REPO, "P2", "src", "soe.h")


def make_batch(lo: int, hi: int, code: str = "FSM_TRANSITION") -> str:
    """确定性事件流：第 i 条事件 t = 0.1*i（一条一行，与 P2 导出口径一致）。"""
    out = [LR.SOE_HEADER]
    for i in range(lo, hi):
        out.append(f"{i * 0.1!r},{i * 0.1!r},0.0,1,INFO,SYSTEM,{code},"
                   f"\"状态迁移 #{i}\",\"\"")
    return "\n".join(out) + "\n"


def naive_current_only(rt: LR.Rotator):
    """[D]/[E] 的错误实现：只看当前段（等价于"忘了扫封存段"）。"""
    return rt.query()


def main() -> int:
    H.main_guard()
    t = T("19/ log_rotator 自检")
    H.banner("19/ log_rotator —— 日志落盘与轮转")

    tmp = tempfile.mkdtemp(prefix="ems19_log_")
    try:
        # =============================================================
        t.section("A 列名契约与解析容忍")
        # =============================================================
        t.eq(LR.SOE_HEADER,
             "t_first,t_last,duration_s,repeat,level,source,code,message,data",
             "SOE 列名与 P2 导出口径一致")
        if os.path.isfile(P2_SOE_H):
            src = open(P2_SOE_H, "r", encoding="utf-8", errors="replace").read()
            m = re.search(r'return "([a-z_,]+)";', src)
            t.guard(m is not None, "在 P2/src/soe.h 里找到了 csv_header() 的字面量")
            t.eq(m.group(1) if m else "", LR.SOE_HEADER,
                 "★ 列名与 P2 SoeLog::csv_header() 逐字一致（列名即契约）")
        else:
            t.skip("未找到 P2/src/soe.h —— 跳过列名契约交叉比对")

        mixed = ("# batch 2026-09-26 10:00:00 reason=open\n" + LR.SOE_HEADER + "\n"
                 "1.0,1.0,0.0,1,INFO,SYSTEM,FSM_TRANSITION,\"迁移\",\"\"\n"
                 "\n"
                 "这一行不是 CSV,只有两列,1\n"
                 "2.0,2.0,0.0,1,ERROR,COMM,COMM_LOST_PCS,\"PCS 通信丢失\",\"\"\n")
        evs = LR.parse_soe_csv(mixed)
        t.eq(len(evs), 2, "表头/注释/短行/空行都被跳过，只剩 2 条事件")
        t.eq(evs[0].code, "FSM_TRANSITION", "首条事件码")
        t.eq(evs[1].level, "ERROR", "次条等级")
        t.eq(LR.parse_soe_csv(""), [], "空输入 → 空结果（不炸）")
        t.eq(LR.parse_soe_csv("garbage\n"), [], "纯垃圾输入 → 空结果（不猜）")

        # =============================================================
        t.section("B 判据①：超过大小阈值触发轮转")
        # =============================================================
        root_b = os.path.join(tmp, "b")
        rb = LR.Rotator(root_b, "soe", max_bytes=1200, max_entries=10 ** 6, keep=5)
        r1 = rb.ingest_soe(text=make_batch(0, 10))
        t.eq(r1["appended"], 10, "第一批 10 条已追加")
        t.eq(r1["rotated"], False, "未超阈值 → 不轮转")
        r2 = rb.ingest_soe(text=make_batch(10, 20))
        t.eq(r2["rotated"], True, "① 大小超阈值 → 触发轮转")
        t.contains(r2["rotated_why"], "bytes=", "① 轮转原因里报了字节数")
        t.eq(r2["sealed"], ["soe_0001.csv"], "① 封存段命名按序号")
        t.lt(rb.current_bytes(), 1200, "① 新段已归零（只有块头）")
        # 反向守卫：阈值放大到足以装下两批时，就不该轮转
        rb2 = LR.Rotator(os.path.join(tmp, "b2"), "soe",
                         max_bytes=10 ** 6, max_entries=10 ** 6, keep=5)
        rb2.ingest_soe(text=make_batch(0, 10))
        t.eq(rb2.ingest_soe(text=make_batch(10, 20))["rotated"], False,
             "① 反向守卫：阈值够大时**不**轮转（说明触发的是阈值而不是别的）")

        # =============================================================
        t.section("C 判据②：条数阈值 + 保留份数（删最旧）")
        # =============================================================
        root_c = os.path.join(tmp, "c")
        rc = LR.Rotator(root_c, "soe", max_bytes=10 ** 9, max_entries=10, keep=2)
        for k in range(0, 100, 10):
            rc.ingest_soe(text=make_batch(k, k + 10))
        sealed = [os.path.basename(p) for _, p in rc.sealed()]
        t.eq(len(sealed), 2, "② 保留份数 == keep（多余的被删）")
        st = rc.stats()
        t.eq(st["sealed_files"], 2, "② stats 里的封存段数与之相符")
        # 10 批 × 10 条、阈值 10 条 → 每两批封一段，共封 5 段（0001..0005）；
        # keep=2 → 只留最新两段
        t.eq(sealed, ["soe_0004.csv", "soe_0005.csv"],
             "② 保留的是**最新两段**（序号 4/5）；最旧的 0001..0003 已删")
        t.ok(not os.path.isfile(os.path.join(root_c, "log", "soe", "soe_0001.csv")),
             "② 最旧的那一段文件确实不在了")
        # 反向守卫：keep 调大时文件就都在
        root_c2 = os.path.join(tmp, "c2")
        rc2 = LR.Rotator(root_c2, "soe", max_bytes=10 ** 9, max_entries=10, keep=20)
        for k in range(0, 100, 10):
            rc2.ingest_soe(text=make_batch(k, k + 10))
        t.guard(len(rc2.sealed()) == 5,
                "② 反向守卫：keep=20 时确实产生了 5 段（说明上面是 keep 在生效）")
        t.eq([os.path.basename(p) for _, p in rc2.sealed()],
             ["soe_0001.csv", "soe_0002.csv", "soe_0003.csv",
              "soe_0004.csv", "soe_0005.csv"],
             "② keep 够大时 5 段全在（未被误删）")
        t.ok(os.path.isfile(os.path.join(root_c2, "log", "soe", "soe_0001.csv")),
             "② 反向守卫：keep=20 时最旧的 0001 仍在")

        # =============================================================
        t.section("D 判据③：不丢事件（确定性事件流逐条可比）")
        # =============================================================
        root_d = os.path.join(tmp, "d")
        N = 200
        BATCH = 20
        # 阈值 25 > 单批 20：于是"每两批封一段" → 事件真的跨段，且当前段非空
        rd = LR.Rotator(root_d, "soe", max_bytes=10 ** 9, max_entries=25, keep=20)
        for k in range(0, N, BATCH):
            rd.ingest_soe(text=make_batch(k, k + BATCH))
        t.eq(len(rd.sealed()), 5, "③ 共产生 5 个封存段 + 当前段（事件确实跨了段）")
        t.guard(len(rd.sealed()) >= 2,
                "③ 反向守卫：事件流确实跨了多段（否则'不丢'没有意义）")
        t.eq(rd.stats()["current_entries"], 0,
             "③ 当前段为空（最后一批正好把阈值顶过，全部封存）")

        all_evs = rd.query()
        t.eq(len(all_evs), N, "③ 落盘后事件总数 == 送入的 200 条（一条不少）")
        want = [f"状态迁移 #{i}" for i in range(N)]
        t.eq([e.message for e in all_evs], want, "③ 逐条可比：消息序列完全一致")
        t.eq([e.t_first for e in all_evs], [i * 0.1 for i in range(N)],
             "③ 逐条可比：时间戳序列完全一致（含浮点精确值）")
        t.eq(len(set(e.key() for e in all_evs)), N, "③ 无重复、无遗漏（键唯一）")

        # 错误实现对照：只看当前段会丢掉绝大部分
        cur_only = list(LR.parse_soe_csv(
            open(rd.current, "r", encoding="utf-8", newline="").read()))
        t.lt(len(cur_only), N, "★ 只扫当前段会漏（说明'不丢'不是自动成立的）")
        t.eq(len(cur_only), 0, "★ 此刻当前段是空的 —— 只看它会一条都查不到")
        t.ok(len(all_evs) - len(cur_only) == N,
             f"★ 全部 {N} 条都只存在于封存段里（差异 {len(all_evs) - len(cur_only)} 条）")

        # 保留窗口的**边界**（诚实记录）：keep 之外的会被删
        t.ok(rc.query(), "（对照）keep=2 的实例里仍有事件可查")
        gone = [i for i in range(0, 20) if f"状态迁移 #{i}" not in
                [e.message for e in rc.query()]]
        t.gt(len(gone), 0, "③ 边界：超出保留窗口的最旧事件确实查不到了（设计如此）")

        # =============================================================
        t.section("E 判据④：轮转后仍能按时间范围查到旧事件")
        # =============================================================
        # 第 0..39 条（t = 0.0 .. 3.9）落在最早那个封存段里（一段 = 两批）
        old = rd.query(t0=0.0, t1=1.9)
        t.eq(len(old), 20, "④ 查 [0.0,1.9] 命中 20 条整体落在最早封存段的事件")
        t.eq(old[0].message, "状态迁移 #0", "④ 能查到最早的那条（不是只有最新的）")
        t.eq(old[-1].message, "状态迁移 #19", "④ 区间右端也正确")
        # ★ 浮点边界：第 19 条的存储值其实是 1.9000000000000001（19*0.1），
        #   没有 T_EPS_S 容差时它会被排除 —— 所以这条断言是有区分度的。
        t.close(old[-1].t_first, 1.9, "④ 右端事件的时间戳确实是 1.9（浮点近似）", tol=1e-12)
        t.ok(old[-1].t_first > 1.9, "④ 存储值是 1.9000000000000001（> 1.9）")
        seg0 = os.path.join(rd.dir, "soe_0001.csv")
        t.ok(os.path.isfile(seg0), "④ 该事件确实在封存段 soe_0001.csv 里（不在当前段）")
        t.eq(len(LR.parse_soe_csv(
            open(seg0, "r", encoding="utf-8", newline="").read())), 2 * BATCH,
            "④ 封存段内容完整可读（40 条 = 累积两批）")
        # ★ 反向守卫：换成"无容差"的比较，边界那一条就没了 —— 证明容差真的在起作用
        naive = [e for e in all_evs if e.t_first <= 1.9]
        t.eq(len(naive), 19, "★ 无容差实现只命中 19 条（漏掉边界那条），差异可见")
        t.eq(len(old) - len(naive), 1, "★ 容差恰好补回 1 条")

        mid = rd.query(t0=9.85, t1=10.05)
        t.eq([e.message for e in mid], ["状态迁移 #99", "状态迁移 #100"],
             "④ 跨段边界查询不漏（#99/#100 分属相邻两段）")
        t.eq(rd.query(t0=100.0, t1=200.0), [], "④ 窗口外 → 空（不是'全都返回'）")

        # 区间相交语义：合并事件跨窗口时必须能被查到
        cross = ("t_first,t_last,duration_s,repeat,level,source,code,message,data\n"
                 "5.0,50.0,45.0,838,WARN,SAFETY,SAFETY_CLIP_START,\"限幅\",\"\"\n")
        rcross = LR.Rotator(os.path.join(tmp, "cross"), "soe")
        rcross.ingest_soe(text=cross)
        t.eq(len(rcross.query(t0=20.0, t1=25.0)), 1,
             "④ 跨窗口的合并事件（5.0~50.0）在 [20,25] 内仍被查到")
        t.eq(rcross.query(t0=51.0, t1=60.0), [], "④ 完全在窗口之后 → 不命中")

        # 维度过滤
        t.eq(len(rd.query(level="ERROR")), 0, "④ 按等级过滤（本流全是 INFO）")
        t.eq(len(rd.query(code="FSM_TRANSITION")), N, "④ 按事件码过滤")
        t.eq(len(rd.query(source="COMM")), 0, "④ 按来源过滤")

        # =============================================================
        t.section("F 接漏：summary.dropped 必须报出来")
        # =============================================================
        root_f = os.path.join(tmp, "f")
        rf = LR.Rotator(root_f, "soe")
        jp = os.path.join(tmp, "soe.json")
        with open(jp, "w", encoding="utf-8") as f:
            json.dump({"summary": {"total_pushed": 5000, "stored": 4096,
                                   "suppressed": 12, "dropped": 3},
                       "events": []}, f, ensure_ascii=False)
        res = rf.ingest_soe(text=make_batch(0, 5), json_path=jp, interval_s=60.0)
        t.eq(res["dropped_in_batch"], 3, "F 从 soe.json 读到 dropped=3")
        t.eq(res["suppressed_in_batch"], 12, "F 抑制次数是**另一回事**（12 次，不算丢失）")
        t.eq(rf.dropped_seen, 3, "F 累计淘汰数记在实例上")
        t.eq(rf.last_interval_s, 60.0, "F 记录本批导出间隔（供算式复核）")
        # 没给 json 时不能凭猜报丢失
        res2 = rf.ingest_soe(text=make_batch(5, 10))
        t.eq(res2["dropped_in_batch"], 0, "F 未提供 json → 不报丢失（不乱猜）")
        t.eq(rf.stats()["dropped_seen"], 3, "F 累计值未被清零")
        # 坏 json 不能把 ingest 弄挂
        bad = os.path.join(tmp, "bad.json")
        open(bad, "w", encoding="utf-8").write("{ not json")
        t.eq(rf.ingest_soe(text=make_batch(10, 12), json_path=bad)["dropped_in_batch"],
             0, "F 坏 json → 当作 0，不抛异常")

        # =============================================================
        t.section("G metrics 也走同一套轮转")
        # =============================================================
        root_g = os.path.join(tmp, "g")
        rg = LR.Rotator(root_g, "metrics", max_bytes=10 ** 9, max_entries=2, keep=1)
        for i in range(5):
            rg.ingest_metrics(text=f"ems_steps_total {1000 + i}\n", label=f"s{i}")
        # 每 3 块封一段（阈值 2，超了才封）→ 封 1 段（0001 = 块 3），当前段留块 4/5
        t.eq(rg.stats()["current_entries"], 2, "G 当前段留 2 块（块 4 与块 5）")
        t.eq(len(rg.sealed()), 1, "G keep=1 → 只留 1 个封存段")
        t.eq(rg.ext, "prom", "G metrics 用 .prom 扩展名")
        body = open(rg.current, "r", encoding="utf-8", newline="").read()
        t.contains(body, "ems_steps_total 1004", "G 当前段是最后一块的内容")
        t.contains(body, "label=s4", "G 块头带 label（可追溯到哪一次导出）")
        sealed_txt = open(rg.sealed()[0][1], "r", encoding="utf-8", newline="").read()
        t.contains(sealed_txt, "# batch", "G 封存段保留了块头")
        t.contains(sealed_txt, "ems_steps_total", "G 封存段保留了指标文本")

        # =============================================================
        t.section("H 导出周期算式 T × r < capacity（含边界反向守卫）")
        # =============================================================
        a = LR.advise(100.0, capacity=4096)
        t.close(a["max_interval_s"], 40.96, "H r=100/s → 最大周期 40.96 s")
        t.close(a["recommended_interval_s"], 20.48, "H 建议周期 = 最大/2")
        b = LR.advise(1.0, capacity=4096)
        t.close(b["max_interval_s"], 4096.0, "H r=1/s → 最大周期 4096 s（= capacity）")
        c = LR.advise(100.0, interval_s=10.0, capacity=4096)
        t.eq(c["safe"], True, "H 边界内（10 s × 100 = 1000 < 4096）→ 安全")
        t.close(c["margin"], 3096.0, "H 余量 3096 条")
        d = LR.advise(100.0, interval_s=60.0, capacity=4096)
        t.eq(d["safe"], False, "H 超出（60 s × 100 = 6000 > 4096）→ 会丢事件")
        t.lt(d["margin"], 0, "H 余量为负")
        # ★ 边界两侧：正好等于 capacity 时**不安全**（淘汰发生在超出的那一刻）
        e = LR.advise(100.0, interval_s=40.0, capacity=4096)
        t.eq(e["safe"], True, "H 40 s × 100 = 4000 < 4096 → 安全")
        f = LR.advise(100.0, interval_s=40.96, capacity=4096)
        t.eq(f["safe"], False, "H 40.96 s × 100 == 4096 → 正好触界，判为不安全")
        try:
            LR.advise(0.0)
            t.ok(False, "H rate<=0 应当抛错")
        except ValueError:
            t.ok(True, "H rate<=0 → ValueError")

        # =============================================================
        t.section("I CLI 端到端（子进程真跑 log_rotator.py）")
        # =============================================================
        root_i = os.path.join(tmp, "i")
        csvp = os.path.join(tmp, "batch.csv")
        open(csvp, "w", encoding="utf-8", newline="\n").write(make_batch(0, 8))
        json0 = os.path.join(tmp, "ok.json")
        with open(json0, "w", encoding="utf-8") as f:
            json.dump({"summary": {"dropped": 0, "suppressed": 4}}, f)
        script = os.path.join(H.SRC, "log_rotator.py")

        r = subprocess.run([sys.executable, script, "--root", root_i,
                            "--max-entries", "5", "ingest", "--csv", csvp,
                            "--json", json0, "--interval", "30"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = r.stdout.decode("utf-8", "replace")
        t.eq(r.returncode, 0, "I ingest 退出码 0")
        t.contains(out, "[ROTATE]", "I 输出里报告了轮转（8 > 5 条阈值）")
        t.contains(out, "本批抑制合并", "I 输出里区分了'抑制'与'丢失'")

        r = subprocess.run([sys.executable, script, "--root", root_i, "status"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        t.eq(r.returncode, 0, "I status 退出码 0")
        t.contains(r.stdout.decode("utf-8", "replace"), "保留窗口",
                   "I status 打印了保留时间窗口")

        r = subprocess.run([sys.executable, script, "--root", root_i, "query",
                            "--from", "0.0", "--to", "0.5"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        t.eq(r.returncode, 0, "I query 退出码 0")
        qout = r.stdout.decode("utf-8", "replace")
        t.contains(qout, LR.SOE_HEADER, "I query 输出 CSV 表头")
        t.contains(qout, "状态迁移 #0", "I query 查到了封存段里的最早事件")

        # dropped>0 必须让 CLI 以 2 退出（把'丢过事件'变成**可被脚本判定的信号**）
        with open(jp, "w", encoding="utf-8") as f:
            json.dump({"summary": {"dropped": 7}}, f)
        r = subprocess.run([sys.executable, script, "--root", root_i, "ingest",
                            "--csv", csvp, "--json", jp],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        t.eq(r.returncode, 2, "I 检出 dropped>0 → 退出码 2（不是 0）")
        t.contains(r.stdout.decode("utf-8", "replace"), "[LOST]",
                   "I 输出里有 [LOST] 醒目行")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    return t.report()


if __name__ == "__main__":
    raise SystemExit(main())
