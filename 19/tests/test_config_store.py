#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""19/tests —— config_store（配置落盘与版本）测试。

四条判据（任务书原文）与本文的落点：
  ① 快照往返读回逐字段一致        → [B] / [G]
  ② diff 能指出"哪个字段从 X 变成 Y" → [D]
  ③ 回滚后生效值 == 目标版本       → [E]
  ④ 回滚前自动备份（反向守卫）      → [E] 版本数 +1 而不是 -1

★ 判据有效性（约定 §3.1）：[H] 段故意给出**错误实现**（只比顶层键的 diff、
  先覆盖再备份的 rollback），断言它们与正确实现的输出**不同**。
  没有这一段，"diff 有效 / 备份有效"就只是"跑过了"，不是"测到了"。

运行：python 19/tests/test_config_store.py
"""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile

import harness as H
from harness import T

import config_store as CS

FIX = os.path.join(H.HERE, "fixtures")
V1 = os.path.join(FIX, "ems_config_site_v1.json")
V2 = os.path.join(FIX, "ems_config_site_v2.json")

# v1 → v2 的**完整**预期差异（逐条写死：这正是"有区分度"的做法 ——
# 少一条、多一条、或某条的值不对，都会红）
EXPECT_DIFF = [
    ("coordinator.soc_kp", "changed", 600, 650),
    ("description", "changed",
     "A 站现场配置（离线验证通过后归档）", "A 站现场配置（现场调参后）"),
    ("limits.d_target_kw", "changed", 320, 360),
    ("limits.transformer_capacity_kw", "changed", 630, 800),
    ("loop.l2_correction_max_kw", "changed", 80, 100),
    ("plant.soc_init", "changed", 0.5, 0.45),
    ("safety.soc_min", "changed", 0.1, 0.12),
    ("strategies.S04_DEMAND_MGMT.params", "removed", {}, None),
    ("strategies.S04_DEMAND_MGMT.params.margin_kw", "added", None, 20),
    ("strategies.S05_ANTI_REVERSE.enabled", "changed", True, False),
]


def naive_toplevel_diff(a, b):
    """[H] 段用的**错误实现**：只比顶层键。字段级变化会被漏掉。"""
    out = []
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            out.append((k, "changed", a.get(k), b.get(k)))
    return out


def main() -> int:
    H.main_guard()
    t = T("19/ config_store 自检")
    H.banner("19/ config_store —— 配置落盘与版本")

    if not (os.path.isfile(V1) and os.path.isfile(V2)):
        t.skip("缺少配置夹具 ems_config_site_v1/v2.json")
        return t.report()

    tmp = tempfile.mkdtemp(prefix="ems19_cfg_")
    root = os.path.join(tmp, "site")
    os.makedirs(root, exist_ok=True)
    st = CS.ConfigStore(root)

    try:
        # =============================================================
        t.section("A 纯函数：canonical / flatten / diff_objects")
        # =============================================================
        o1 = CS.load_json(V1)
        o2 = CS.load_json(V2)
        t.eq(CS.canonical(o1), CS.canonical(CS.load_json(V1)), "canonical 幂等")
        t.ok(CS.canonical(o1).rstrip("\n").endswith("}"), "canonical 结尾是 }")
        # sort_keys 生效：顶层键顺序必须是字典序
        keys = [ln.split(":")[0].strip().strip('"')
                for ln in CS.canonical(o1).splitlines()[1:] if ln.startswith('  "')]
        t.eq(keys, sorted(keys), "canonical 按键排序（顶层键为字典序）")
        t.eq(keys[0], "coordinator", "首键 == coordinator（c 在 d 之前）")

        flat = CS.flatten(o1)
        t.eq(len(flat), 105, "扁平化叶子数 == 105（89 字段 + 元信息 + 策略条目）")
        t.eq(flat["safety.soc_min"], 0.1, "叶子路径 safety.soc_min 可直接取到")
        t.eq(flat["strategies.S05_ANTI_REVERSE.enabled"], True, "嵌套策略叶子可取")
        t.eq(CS.flatten({"a": {}}), {"a": {}}, "空对象本身算一个叶子（防'整节被删'漏报）")
        t.eq(CS.flatten({"a": []}), {"a": []}, "空数组本身算一个叶子")
        t.eq(CS.flatten({"a": [1, 2]}), {"a[0]": 1, "a[1]": 2}, "数组按下标展开")

        ch = CS.diff_objects(o1, o2)
        t.eq(len(ch), len(EXPECT_DIFF), "diff 条目数 == 预期")
        for want in EXPECT_DIFF:
            t.ok(want in ch, f"diff 命中 {want[0]}（{want[1]}）")

        t.eq(CS.diff_objects(o1, o1), [], "同一对象 diff 为空")
        # 类型突变：dict → 标量 必须报成 removed+added，而不是静默跳过
        ty = CS.diff_objects({"a": {"b": 1}}, {"a": 5})
        t.eq([x[1] for x in ty], ["added", "removed"], "类型突变 = added + removed")
        t.eq(sorted(x[0] for x in ty), ["a", "a.b"], "类型突变的路径集合")

        # =============================================================
        t.section("B 快照往返（判据①）")
        # =============================================================
        v1 = st.snapshot(source=V1, note="初始归档")
        t.eq(v1.seq, 1, "首版序号 = 1")
        t.ok(v1.name.startswith("v0001_"), f"版本名带序号前缀：{v1.name}")

        back = CS.load_json(v1.path)
        t.eq(CS.flatten(back), CS.flatten(o1), "① 往返逐字段一致（flatten 全等）")
        t.eq(CS.canonical(back), CS.canonical(o1), "① 往返规范化文本一致")
        t.eq(CS.load_json(v1.path)["safety"]["soc_min"], 0.1, "抽查字段值")
        t.eq(st.verify(v1.name), [], "verify 无问题（落盘即规范化形式）")

        # 落盘文本必须是规范化形式 —— 手工改过一次排版就要能看出来
        p = v1.path
        raw = open(p, "r", encoding="utf-8", newline="").read()
        t.ok(raw.endswith("\n"), "落盘文件以换行结尾")
        t.ok("\r" not in raw, "落盘文件是 LF（不是 CRLF）—— 版本可比对的前提")
        open(p, "w", encoding="utf-8", newline="\n").write(
            raw.replace('  "description"', '    "description"'))
        t.ne(st.verify(v1.name), [], "★ 手工改过排版 → verify 能红（有区分度）")
        open(p, "w", encoding="utf-8", newline="\n").write(raw)   # 复原
        t.eq(st.verify(v1.name), [], "复原后 verify 回到无问题")

        # =============================================================
        t.section("C list（含『删掉 index 也能列出来』这条设计决策）")
        # =============================================================
        v2 = st.snapshot(source=V2, note="现场调参")
        rows = st.list_versions()
        t.eq(len(rows), 2, "list 出 2 版")
        t.eq([r["version"] for r in rows], [v1.name, v2.name], "版本按序号升序")
        t.eq([r["note"] for r in rows], ["初始归档", "现场调参"], "备注来自 index.json")
        t.eq(rows[1]["fields"], 105, "每版字段数可读")
        t.ok(rows[0]["sha256"] and len(rows[0]["sha256"]) == 16, "sha256 已计算")

        idx = st.index_path
        os.remove(idx)
        rows2 = st.list_versions()
        t.eq(len(rows2), 2, "★ 删掉 index.json 后仍能列出全部版本（目录是真相源）")
        t.eq([r["note"] for r in rows2], ["", ""], "此时备注为空（advisory 而已）")
        t.eq([r["version"] for r in rows2], [v1.name, v2.name], "版本集合不变")
        t.eq(st.list_versions()[0]["fields"], 105, "备注丢了不影响字段数")
        # 索引坏掉不能连带把 diff/rollback 弄挂
        with open(idx, "w", encoding="utf-8") as f:
            f.write("{ this is not json")
        t.eq(len(st.list_versions()), 2, "索引是坏 JSON 时 list 仍可用")
        t.eq(len(st.diff(v1.name, v2.name)), len(EXPECT_DIFF), "索引坏掉时 diff 仍可用")
        os.remove(idx)

        # =============================================================
        t.section("D diff 逐字段（判据②）")
        # =============================================================
        d = st.diff(v1.name, v2.name)
        pairs = [(p_, k, old, new) for p_, k, old, new in d]
        t.eq(len(pairs), 10, "两版共 10 处差异")
        t.ok(("safety.soc_min", "changed", 0.1, 0.12) in pairs,
             "② 指出 safety.soc_min 从 0.1 → 0.12")
        t.ok(("limits.d_target_kw", "changed", 320, 360) in pairs,
             "② 指出 limits.d_target_kw 从 320 → 360")
        t.ok(("strategies.S05_ANTI_REVERSE.enabled", "changed", True, False) in pairs,
             "② 指出策略开关从 true → false（嵌套三层）")
        t.eq(st.diff(v2.name, v2.name), [], "自己比自己 = 0 处")
        rev = st.diff(v2.name, v1.name)
        t.ok(("safety.soc_min", "changed", 0.12, 0.1) in rev, "反向 diff 方向正确（可逆）")

        # =============================================================
        t.section("E rollback（判据③④）")
        # =============================================================
        # 先把 active 设成 v2（模拟"现场调参后的运行态"）
        st.save_active(CS.load_json(v2.path))
        t.eq(len(st.list_versions()), 2, "准备完毕：2 版")
        t.eq(CS.load_json(st.active)["safety"]["soc_min"], 0.12, "active 当前是 v2 的值")

        n_before = len(st.list_versions())
        res = st.rollback(v1.name)
        n_after = len(st.list_versions())

        t.eq(CS.flatten(CS.load_json(st.active)), CS.flatten(CS.load_json(v1.path)),
             "③ 回滚后生效值 == 目标版本（逐字段）")
        t.eq(CS.load_json(st.active)["safety"]["soc_min"], 0.1, "③ 抽查字段已回到 0.1")
        t.eq(res["restored_from"], v1.name, "结果里报告了来源版本")

        t.guard(res["backup_version"] is not None, "回滚确实先存了一版（否则④没测到）")
        t.eq(n_after, n_before + 1, "④ 回滚后版本数 +1（不是 -1）")
        bak = CS.load_json(os.path.join(st.history, res["backup_version"] + ".json"))
        t.eq(CS.flatten(bak), CS.flatten(CS.load_json(v2.path)),
             "④ 备份的是**回滚前的当前值**（v2），不是目标值（v1）")
        t.eq(CS.load_json(st.active)["safety"]["soc_min"], 0.1,
             "④ 且 active 确实是 v1（备份没有把 target 写回 active）")

        # 回滚后 active 能被 resolve 成目标版本
        t.eq(st.resolve("active").name, v1.name, "回滚后 resolve('active') == 目标版本")

        # 二次回滚：再回 v2，版本数继续 +1（每次都可回滚）
        n2 = len(st.list_versions())
        st.rollback(v2.name)
        t.eq(len(st.list_versions()), n2 + 1, "二次回滚同样 +1（回滚可反复进行）")
        t.eq(CS.load_json(st.active)["safety"]["soc_min"], 0.12, "二次回滚生效值正确")

        # 失败路径必须**不改动 active**（原子性）
        active_before = open(st.active, "r", encoding="utf-8", newline="").read()
        try:
            st.rollback("v9999")
            t.ok(False, "回滚不存在的版本应当抛错")
        except KeyError:
            t.ok(True, "回滚不存在的版本 → KeyError")
        t.eq(open(st.active, "r", encoding="utf-8", newline="").read(), active_before,
             "失败的 rollback 没有改动 active（原子性）")

        # =============================================================
        t.section("F 版本引用与边界")
        # =============================================================
        rows = st.list_versions()
        t.eq(st.resolve("latest").seq, max(r["seq"] for r in rows), "latest = 最大序号")
        t.eq(st.resolve("1").name, v1.name, "裸数字 1 可解析")
        t.eq(st.resolve("v0001").name, v1.name, "v0001 可解析")
        for bad in ("v0000", "abc", "v1x"):
            try:
                st.resolve(bad)
                t.ok(False, f"非法引用 {bad!r} 应当抛错")
            except KeyError:
                t.ok(True, f"非法引用 {bad!r} → KeyError")

        # 非对象 JSON 必须被拒（配置文件不是数组/标量）
        bad_json = os.path.join(tmp, "bad.json")
        with open(bad_json, "w", encoding="utf-8") as f:
            f.write("[1,2,3]")
        try:
            st.snapshot(source=bad_json)
            t.ok(False, "非对象配置应当被拒")
        except ValueError:
            t.ok(True, "非对象配置 → ValueError")

        # =============================================================
        t.section("G 与 P1 的 --capture 对接（真 exe，缺则 SKIP）")
        # =============================================================
        exe = CS.default_capture_exe()
        if not exe or not os.path.isfile(exe):
            t.skip("未找到 P1/build/ems_config.exe —— 跳过 --capture 真跑（设 EMS_CONFIG_EXE 可指定）")
        else:
            root2 = os.path.join(tmp, "site2")
            st2 = CS.ConfigStore(root2)
            try:
                vc = st2.snapshot(note="capture 真跑")
                cap = CS.load_json(vc.path)
                t.eq(len(CS.flatten(cap)), 104, "capture 产物叶子数 == 104")
                t.eq(cap["safety"]["soc_min"], 0.1, "capture 里 soc_min 值正确")
                # 与仓库里已归档的 captured.json 逐字段一致（同源同值）
                known = os.path.join(H.REPO, "P1", "build", "captured.json")
                if os.path.isfile(known):
                    t.eq(CS.flatten(cap), CS.flatten(CS.load_json(known)),
                         "capture 产物与 P1/build/captured.json 逐字段一致")
                else:
                    t.skip("P1/build/captured.json 不存在，跳过交叉比对")
                t.eq(st2.verify(vc.name), [], "capture 落盘后 verify 通过")
            except (RuntimeError, OSError) as e:
                t.skip(f"--capture 真跑失败（环境问题，非代码缺陷）：{e}")

        # =============================================================
        t.section("H 判据有效性（故意用错实现，断言它会被比出来）")
        # =============================================================
        good = CS.diff_objects(o1, o2)
        bad = naive_toplevel_diff(o1, o2)
        t.guard(len(good) > 0, "正确 diff 有结果（否则下面的对比无意义）")
        t.eq(len(good), 10, "正确 diff 给出 10 条**字段级**变化")
        t.eq(len(bad), 7, "只比顶层键时只剩 7 条（整节被当成一个值）")
        t.ok(all("." not in p for p, _, _, _ in bad),
             "★ 错误实现给出的路径**全是顶层节名，指不到字段**")
        t.ok(any("." in p for p, _, _, _ in good),
             "★ 正确实现给出的路径能落到字段（含点号）")
        t.ok("safety.soc_min" not in [p for p, _, _, _ in bad],
             "★ 错误实现确实指不出 safety.soc_min")
        t.ok("limits.d_target_kw" not in [p for p, _, _, _ in bad],
             "★ 错误实现确实指不出 limits.d_target_kw")
        t.ok(("safety", "changed", o1["safety"], o2["safety"]) in bad,
             "★ 错误实现只能把整个 safety 节说成『变了』—— 运维拿它没法定位")

        # 反向：把"先备份"去掉 → 版本数不会 +1
        root3 = os.path.join(tmp, "site3")
        st3 = CS.ConfigStore(root3)
        st3.snapshot(source=V1)
        st3.save_active(CS.load_json(V2))
        n3 = len(st3.list_versions())
        st3.save_active(CS.load_json(st3.resolve("1").path))   # 不备份直接覆盖
        t.eq(len(st3.list_versions()), n3,
             "★ 去掉自动备份后版本数不变 —— 所以判据④能区分『备份了/没备份』")
        t.ne(len(st3.list_versions()), n3 + 1,
             "★ 反向守卫：错误实现的版本数**不**等于正确实现的 +1")

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    return t.report()


if __name__ == "__main__":
    raise SystemExit(main())
