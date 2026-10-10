# -*- coding: utf-8 -*-
"""告警分类探针：既量维度区分度，也端到端验证 /api/alarms 与 /api/alarms/summary。

为什么留这个脚本
----------------
15/ 的自检是**静态**的（扫源码正则），它只能证明"前端传了 level/source 这两个参数"，
证明不了"后端真按这两个参数筛出了对的行"。而告警筛选出错是静默的：
SQL 条件拼错不报错，只是条数为 0 —— 界面上"筛完没有"和"真的没有"长得一样。

所以这里拿 14/data/ems.db 真库跑一遍，把每个组合的条数打出来，
并用**独立 SQL** 当标尺对拍。结论写进 .probe_alarm.out。

用法：
    python 14/scripts/probe_alarm_dims.py
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), "src")
sys.path.insert(0, SRC)

import api      # noqa: E402
import auth     # noqa: E402
import emsdb    # noqa: E402

DB = os.path.join(os.path.dirname(HERE), "data", "ems.db")
out = []


def main():
    conn = emsdb.connect(DB)
    # 真库里的 admin 口令可能已被现场改掉，认证失败会返回 None → 后续全是 401，
    # 看起来像"筛选坏了"。这里显式兜底并**把用的是哪种身份写进结果**，
    # 免得结论被身份问题污染。
    admin = auth.authenticate(conn, "admin", "admin123")
    if admin:
        out.append(f"身份：库内 admin（user_id={admin.get('user_id')}）")
    else:
        admin = {"user_id": 0, "username": "probe", "role": "admin"}
        out.append("身份：库内 admin 认证未通过（口令已被改），改用合成 admin 继续")

    def call(path, **query):
        q = {k: [str(v)] for k, v in query.items()}
        st, body = api.dispatch(conn, "GET", path, q, {}, admin)
        if st != 200:
            out.append(f"     !! {path} {query} → 状态码 {st}，返回 {body!r}")
        return st, body

    def sql_n(sc, **cond):
        w, a = "scenario_id = ?", [sc]
        if cond.get("level"):
            w += " AND level = ?"
            a.append(cond["level"])
        if cond.get("source"):
            w += " AND source = ?"
            a.append(cond["source"])
        return emsdb.query_value(conn, f"SELECT COUNT(*) FROM alarm WHERE {w}", a, -1)

    out.append("===== ① 各维度区分度（决定前端该开放哪几个筛选项） =====")
    total = emsdb.query_value(conn, "SELECT COUNT(*) FROM alarm", (), 0)
    out.append(f"alarm 总行数 = {total}")
    for col in ("level", "source", "status", "device_id", "alarm_type", "scenario_id"):
        rows = emsdb.query(
            conn, f"SELECT {col} AS k, COUNT(*) AS n FROM alarm"
                  f" GROUP BY {col} ORDER BY n DESC")
        out.append(f"  {col:12} {len(rows)} 个取值: "
                   + ", ".join(f"{r['k']}={r['n']}" for r in rows[:8]))
        if len(rows) == 1:
            out.append(f"               ^^^ 单值维度 —— 开放成筛选项就是『恒 0 条』假入口")

    out.append("\n===== ② /api/alarms 筛选：处理器 total vs 独立 SQL =====")
    scs = [r["scenario_id"] for r in emsdb.query(
        conn, "SELECT DISTINCT scenario_id FROM alarm ORDER BY scenario_id")]
    bad = 0
    for sc in scs:
        lvs = [r["level"] for r in emsdb.query(
            conn, "SELECT DISTINCT level FROM alarm WHERE scenario_id=? ORDER BY level", (sc,))]
        srcs = [r["source"] for r in emsdb.query(
            conn, "SELECT DISTINCT source FROM alarm WHERE scenario_id=? ORDER BY source", (sc,))]
        combos = ([{}] + [{"level": x} for x in lvs] + [{"source": y} for y in srcs]
                  + [{"level": x, "source": y} for x in lvs for y in srcs]
                  + [{"level": "", "source": ""}])
        out.append(f"\n  -- 场景 {sc}（等级 {lvs} × 来源 {srcs}，共 {len(combos)} 个组合）")
        for c in combos:
            st, body = call("/api/alarms", scenario=sc, limit=50, **c)
            want = sql_n(sc, **c)
            got = body.get("total") if isinstance(body, dict) else None
            ok = (st == 200 and got == want)
            if not ok:
                bad += 1
            out.append(f"     {'ok  ' if ok else 'FAIL'} {str(c):44} "
                       f"total={str(got):>5}  SQL={want:>5}")

    out.append("\n===== ③ 交叉筛选恒等式（各维度计数带‘另一个维度’，不带自己） =====")
    for sc in scs:
        for c in ([{}] + [{"level": "FAULT"}] + [{"source": "COMM"}]
                  + [{"level": "FAULT", "source": "COMM"}]):
            if c.get("level") and c["level"] not in [r["level"] for r in emsdb.query(
                    conn, "SELECT DISTINCT level FROM alarm WHERE scenario_id=?", (sc,))]:
                continue
            if c.get("source") and c["source"] not in [r["source"] for r in emsdb.query(
                    conn, "SELECT DISTINCT source FROM alarm WHERE scenario_id=?", (sc,))]:
                continue
            _, sm = call("/api/alarms/summary", scenario=sc, **c)
            if not isinstance(sm, dict) or "by_level" not in sm:
                out.append(f"  -- [{sc}] {c} summary 返回异常：{sm!r}")
                bad += 1
                continue
            lv_sum = sum(r["n"] for r in sm["by_level"])
            sr_sum = sum(r["n"] for r in sm["by_source"])
            tl_sum = sum(r["n"] for r in sm["timeline"])
            w_lv = sql_n(sc, **({"source": c["source"]} if "source" in c else {}))
            w_sr = sql_n(sc, **({"level": c["level"]} if "level" in c else {}))
            w_tl = sql_n(sc, **c)
            out.append(f"  -- [{sc}] {c or '不筛'}")
            out.append(f"     by_level 之和 {lv_sum:>4} vs 只按来源筛 {w_lv:>4}  {'ok' if lv_sum == w_lv else 'FAIL'}")
            out.append(f"     by_source之和 {sr_sum:>4} vs 只按等级筛 {w_sr:>4}  {'ok' if sr_sum == w_sr else 'FAIL'}")
            out.append(f"     timeline之和 {tl_sum:>4} vs 两维都应用 {w_tl:>4}  {'ok' if tl_sum == w_tl else 'FAIL'}")
            out.append(f"     等级全谱: " + ", ".join(f"{r['level']}={r['n']}" for r in sm["by_level"]))
            if lv_sum != w_lv or sr_sum != w_sr or tl_sum != w_tl:
                bad += 1

    out.append(f"\n不符项合计 = {bad}")
    conn.close()

    with open(os.path.join(os.path.dirname(HERE), ".probe_alarm.out"), "w",
              encoding="utf-8") as f:
        f.write("\n".join(out))


if __name__ == "__main__":
    main()
