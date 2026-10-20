"""14/ — 「接入调度主站」九步向导：字段定义、草稿存储、导出

★ 为什么字段定义只在后端这一份

    §94 的教训是「同一份东西不许两套算法」。九步台账的字段清单如果前端写一份、
    导出时后端再写一份，两处迟早不一致 —— 到时候导出件看着完整，实际漏了界面
    新加的那一列。所以：**定义只在这里**，前端按 GET 回来的 schema 渲染，
    后端按同一份定义做完成校验与 CSV 导出。

★ 为什么草稿存 meta 表而不是在前端 localStorage

    换台机器、换个浏览器都还得在；而且要能回答「谁最后填的、什么时候填的」。
    meta(key, value) 是现成的键值表，不必为这一件事建新表。

★ 为什么「必填」按行业标准定而不是能填就必填

    九步里大量字段属于外部事项（通道、证书、联调结果），拿不到就是拿不到。
    真正该卡的只有两类：
      ① 不填就没法开工的（归口调度、号段、本机 IP）；
      ② 行业资料里必须有、缺了对点报告出不了签字的（DL/T 516-2017 7.2、
         并网调度协议示范文本 5.5 条：变比、满度、报告编号、签字日期）。
    其余一律选填 —— 逼人填假日期比留空更坏。
"""

from __future__ import annotations

import datetime
import json
from typing import Any, Optional

import emsdb

DRAFT_KEY = "access_draft"
DRAFT_VERSION = 1

# ---------------------------------------------------------------------
# 九步定义
#
# 字段类型：text / date / time / int / select / multi / textarea / yesno
#   req      必填。★ 与 dep 同时出现时 req 必须为 True ——
#            dep 表达的是"满足条件时才必填"，不是"永不必填"。
#            写成 req=False + dep 会让这条永远不生效（v1.14 的自检抓到过一次）。
#   dep      条件必填：当 dep.k 的取值落在 dep.in 里时才必填
#   opts     选项（select / multi）：[[值, 显示文案], ...]
#   hint     为什么要问这一项 —— 每格都写，界面直接显示
# ---------------------------------------------------------------------

STEPS: list[dict[str, Any]] = [
    {
        "key": "1",
        "num": 1,
        "title": "管辖权与接入方案",
        "sub": "谁管我们 · 几条通道",
        "why": "这一步决定后面要做几套。储能站常是「地区调度管监视、省调管 AGC/AVC」，"
               "两个口子就是两条独立链路、两套点表、两次联调 —— 开始时没问清，"
               "到联调才发现，前面做的都要重来。",
        "fields": [
            {"k": "dispatch", "label": "归口调度", "type": "text", "req": True,
             "ph": "如：XX 地区电力调度控制中心",
             "hint": "管辖我们的是哪一级调度。资料报给谁、点表谁审，都看这一项。"},
            {"k": "second_scope", "label": "是否另有上级调度调管", "type": "select",
             "req": True,
             "opts": [["no", "无，只对一家调度"], ["avc", "省调调管 AGC / AVC"]],
             "hint": "有的话要再走一套链路与点表。这一项错判是返工最常见的原因。"},
            {"k": "second_name", "label": "上级调度名称", "type": "text", "req": True,
             "dep": {"k": "second_scope", "in": ["avc"]},
             "ph": "如：XX 省电力调度控制中心",
             "hint": "选了「省调调管」就必须写清是哪一家，否则无法确定第二套资料报给谁。"},
            {"k": "mode", "label": "接入方式", "type": "select", "req": True,
             "opts": [["专线", "专线"], ["调度数据网", "调度数据网"]],
             "hint": "两者在通道侧做法不同：数据网要申请接入、分配 IP 与 VLAN。"},
            {"k": "channels", "label": "通道数", "type": "select", "req": True,
             "opts": [["单通道", "单通道"], ["双通道", "双通道（主备）"]],
             "hint": "储能并网普遍要求双通道冗余；我们目前软件侧只有一条 2404 监听。"},
            {"k": "doc", "label": "方案文件 / 会议纪要", "type": "text", "req": False,
             "ph": "文件名或编号",
             "hint": "口头的方案过两个月就没人记得了。有纪要就写在这里，出问题时能追溯。"},
            {"k": "note", "label": "备注", "type": "textarea", "req": False},
        ],
    },
    {
        "key": "2",
        "num": 2,
        "title": "报送资料",
        "sub": "报了什么 · 报给谁",
        "why": "按 DL/T 516-2017 与并网调度协议，报的不只是一张点表，"
               "还要含变比、遥测满度这些「数准不准」的依据。漏报了后面查不出原因。",
        "fields": [
            {"k": "date", "label": "报送日期", "type": "date", "req": True,
             "hint": "以最后一次报送为准；改版重报时改这一项并记进备注。"},
            {"k": "via", "label": "报送方式", "type": "select", "req": True,
             "opts": [["OA", "OA 流转"], ["邮件", "邮件"], ["纸质", "纸质盖章"],
                      ["其他", "其他"]],
             "hint": "OA / 邮件有留痕，纸质要留签收。"},
            {"k": "receiver", "label": "接收人（调度侧）", "type": "text", "req": True,
             "ph": "姓名 / 班组",
             "hint": "改版时能找到对的人，比找到对的单位有用。"},
            {"k": "items", "label": "已报资料", "type": "multi", "req": True,
             "opts": [["rtu", "远动信息表（点表）"],
                      ["tech", "设备技术资料"],
                      ["ratio", "CT / PT 变比与遥测满度"],
                      ["csv", "点表 CSV（机器可读）"],
                      ["other", "其他"]],
             "hint": "「变比与满度」这一项最常被漏，而它决定对点时数值对不对得上。"},
            {"k": "note", "label": "备注（补报 / 缺项）", "type": "textarea", "req": False},
        ],
    },
    {
        "key": "3",
        "num": 3,
        "title": "审改与号段下发",
        "sub": "号段是他们定的",
        "why": "IOA 号段由调度分配，我们只做映射。调度通常会减少点位 —— "
               "我们要报 30 个，他们真正要的可能是 15 个。改 2~3 轮是常态，不是意外。",
        "fields": [
            {"k": "date", "label": "审定日期", "type": "date", "req": True},
            {"k": "segments", "label": "下发号段", "type": "text", "req": True,
             "ph": "如：遥信 1001-1999 / 遥测 4001-4999",
             "hint": "★ 我们当前只认 1xxx 遥信 / 4xxx 遥测 / 5xxx 遥控 / 6xxx 遥调 / "
                     "8xxx 累计量。号段与此不符时表存不进去，要先改规则（C++ 与 "
                     "Python 两处）。"},
            {"k": "up_count", "label": "审定后上送点数", "type": "int", "req": True,
             "hint": "填进软件的点表后，这个数应当与转发表里的上送行数一致。"},
            {"k": "down_count", "label": "审定后下行点数", "type": "int", "req": True},
            {"k": "rounds", "label": "改版轮次", "type": "int", "req": False,
             "hint": "记下来是为了下次估工期 —— 第一版就定稿的情况很少。"},
            {"k": "final", "label": "点表是否定稿", "type": "select", "req": True,
             "opts": [["no", "未定稿"], ["yes", "已定稿（可据此填表）"]],
             "hint": "只有定稿后才该去第 6 步改转发表 —— 拿未定稿改，等于白改一遍。"},
            {"k": "note", "label": "备注（被删掉的点、争议项）", "type": "textarea",
             "req": False},
        ],
    },
    {
        "key": "4",
        "num": 4,
        "title": "通信通道",
        "sub": "能不能路由到我们",
        "why": "104 里我们是服务端、调度是客户端。所以「连不上」的第一件事不是握手，"
               "而是他们的地址能不能到达我们的端口。绝大多数现场问题都在这一步。",
        "fields": [
            {"k": "local_ip", "label": "本机 IP（对调度可见）", "type": "text",
             "req": True, "ph": "如：10.163.13.175",
             "hint": "★ 填的是调度侧能路由到的那个地址。本机的 WLAN 内网地址"
                     "（192.168.x.x / 10.x 办公网）真调度连不进来。"},
            {"k": "mask", "label": "子网掩码", "type": "text", "req": False,
             "ph": "255.255.255.0"},
            {"k": "gw", "label": "网关", "type": "text", "req": False},
            {"k": "master_ip", "label": "调度侧主站 IP", "type": "text", "req": True,
             "hint": "我们不主动连它，所以它不影响通信；但白名单与排障需要它。"
                     "★ 当前软件没有白名单功能，这一项目前只作记录。"},
            {"k": "port", "label": "监听端口", "type": "int", "req": True,
             "default": 2404,
             "hint": "IEC 104 标准端口 2404。改这里只改记录，真正的监听口在第 6 步。"},
            {"k": "vlan", "label": "VLAN", "type": "text", "req": False},
            {"k": "ch_a", "label": "通道 A 已通", "type": "yesno", "req": True},
            {"k": "ch_b", "label": "通道 B 已通", "type": "yesno", "req": True,
             "dep": {"k": "channels", "in": ["双通道"]}},
            {"k": "note", "label": "备注（路由 / 防火墙）", "type": "textarea",
             "req": False},
        ],
    },
    {
        "key": "5",
        "num": 5,
        "title": "安全防护",
        "sub": "证书是日历时间",
        "why": "证书要向调控中心申请，1~2 周起步，赶上集中报批更久。"
               "这一步不占工作量但占日历，必须在报送资料之前就动 —— "
               "绝大多数项目卡在「资料早定了、证书还没下来」。",
        "fields": [
            {"k": "device", "label": "纵向加密认证装置", "type": "text", "req": True,
             "ph": "厂家 / 型号",
             "hint": "104 本身明文，过安全评估要靠这层。我们软件侧没有 TLS。"},
            {"k": "cert_apply", "label": "证书申请日期", "type": "date", "req": True},
            {"k": "cert_exp", "label": "证书到期日", "type": "date", "req": True,
             "hint": "到期会断链。这个日期比申请日期更需要有人盯。"},
            {"k": "assess", "label": "安全评估", "type": "select", "req": True,
             "opts": [["no", "未做"], ["doing", "进行中"], ["yes", "已通过"]],
             "hint": "很多省把这一项作为接入的前置条件，没过不能投运。"},
            {"k": "note", "label": "备注", "type": "textarea", "req": False},
        ],
    },
    {
        "key": "6",
        "num": 6,
        "title": "站内配置",
        "sub": "唯一真改软件的一步",
        "why": "九步里只有这一步会写文件。监听口与 CA 落到 active.upstream，"
               "转发表落到 active.pointmap.csv，网关**下次启动**才读 —— 没有热更新。"
               "改完重启会断 104 链路，调度侧看得到，现场得先报备。",
        "config": True,
        "fields": [
            {"k": "selftest", "label": "已用模拟主站跑通", "type": "select", "req": True,
             "opts": [["no", "未跑"], ["yes", "已跑通（总召 + 逐点值正确）"]],
             "hint": "用自己的 iec104_master 先跑一遍，能排掉一大半配置错误；"
                     "但它跑的是同一套协议、**不是真调度要的数**。"},
            {"k": "selftest_at", "label": "跑通时间", "type": "time", "req": True,
             "dep": {"k": "selftest", "in": ["yes"]}},
            {"k": "note", "label": "备注（重启报备 / 遗留问题）", "type": "textarea",
             "req": False},
        ],
    },
    {
        "key": "7",
        "num": 7,
        "title": "三级联调",
        "sub": "静态 · 闭环 · 动态",
        "why": "行业固定三步法：静态校核专门抓「拖错点」（IOA 对上了、绑的内部点却错了），"
               "闭环测试触发变位看另一头，动态校核验配置一致性。"
               "有自动验收装置时 220kV 站 1~2 天，纯人工 4~5 天。",
        "fields": [
            {"k": "static_date", "label": "静态校核日期", "type": "date", "req": True,
             "hint": "点表 / RCD / 主站库三方比对。我们没有 RCD 能力，这一步靠人工。"},
            {"k": "static_ok", "label": "静态校核结果", "type": "select", "req": True, "opts": [["pass", "通过"], ["fail", "有遗留"]]},
            {"k": "loop_date", "label": "闭环测试日期", "type": "date", "req": True},
            {"k": "loop_ok", "label": "闭环测试结果", "type": "select", "req": True, "opts": [["pass", "通过"], ["fail", "有遗留"]]},
            {"k": "dyn_date", "label": "动态校核日期", "type": "date", "req": True},
            {"k": "dyn_ok", "label": "动态校核结果", "type": "select", "req": True, "opts": [["pass", "通过"], ["fail", "有遗留"]]},
            {"k": "todo", "label": "遗留问题数", "type": "int", "req": True,
             "hint": "填 0 才算真的干净。有遗留就该写进备注，别只留个数字。"},
            {"k": "note", "label": "备注（遗留项 / 处理人）", "type": "textarea",
             "req": False},
        ],
    },
    {
        "key": "8",
        "num": 8,
        "title": "遥控遥调传动",
        "sub": "真动一次才算过",
        "why": "只对点不够：遥控必须让开关真动一次，遥调必须看到功率真变化。"
               "还有一项最容易漏 —— 闭锁试验：模拟通道中断，看停机/闭锁类命令"
               "是否按要求保持，其余命令是否按超时释放。",
        "fields": [
            {"k": "ctl_point", "label": "遥控传动对象", "type": "text", "req": True,
             "ph": "如：5001 PCS 启停", "hint": "写 IOA 与对象，别只写「遥控正常」。"},
            {"k": "ctl_ok", "label": "遥控结果", "type": "select", "req": True, "opts": [["pass", "动作正确"], ["fail", "不动作 / 错动"]]},
            {"k": "adj_point", "label": "遥调传动对象", "type": "text", "req": True,
             "ph": "如：6001 有功设定"},
            {"k": "adj_ok", "label": "遥调结果", "type": "select", "req": True, "opts": [["pass", "功率跟随"], ["fail", "不跟随 / 偏差大"]]},
            {"k": "block_ok", "label": "闭锁试验结果", "type": "select", "req": True,
             "opts": [["pass", "停机/闭锁保持，其余超时释放"],
                      ["fail", "保持不住 或 该释放的没释放"]]},
            {"k": "note", "label": "备注", "type": "textarea", "req": False},
        ],
    },
    {
        "key": "9",
        "num": 9,
        "title": "报告与投运",
        "sub": "签字 · 试运行 · 归档",
        "why": "对点报告双方签字是拿并网调度协议的前置条件之一。"
               "资料按 DL/T 516 归档，之后改点要走变更流程 —— 不是改完就算。",
        "fields": [
            {"k": "report_no", "label": "对点报告编号", "type": "text", "req": True,
             "hint": "没有编号的报告在归档时会被退回重出。"},
            {"k": "sign_date", "label": "双方签字日期", "type": "date", "req": True},
            {"k": "trial_at", "label": "试运行开始", "type": "time", "req": True,
             "hint": "通常要求连续 72 小时以上不出问题。"},
            {"k": "run_at", "label": "正式投运时间", "type": "time", "req": False,
             "hint": "还在试运行就先留空 —— 留空是事实，填个预计日期会变成假记录。"},
            {"k": "archive", "label": "归档位置", "type": "text", "req": True,
             "ph": "如：资料室 D-2026-014 / OA 档案编号"},
            {"k": "note", "label": "备注", "type": "textarea", "req": False},
        ],
    },
]

STEP_BY_KEY = {s["key"]: s for s in STEPS}
ALL_FIELDS = {s["key"]: {f["k"]: f for f in s["fields"]} for s in STEPS}


# ---------------------------------------------------------------------
# 草稿读写
# ---------------------------------------------------------------------
def _now() -> str:
    return datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")


def _blank() -> dict[str, Any]:
    """全新草稿：字段默认值先铺满，前端不用自己补。"""
    steps: dict[str, dict[str, Any]] = {}
    for s in STEPS:
        row: dict[str, Any] = {}
        for f in s["fields"]:
            row[f["k"]] = f.get("default", [] if f["type"] == "multi" else "")
        steps[s["key"]] = row
    return {
        "version": DRAFT_VERSION,
        "current_step": 1,
        "completed": False,
        "completed_at": "",
        "updated_at": "",
        "username": "",
        "steps": steps,
    }


def _clean(steps: Any) -> dict[str, dict[str, Any]]:
    """只保留 schema 里有的键与合法类型 —— 脏数据不许悄悄进库。"""
    out: dict[str, dict[str, Any]] = {}
    src = steps if isinstance(steps, dict) else {}
    for s in STEPS:
        k = s["key"]
        raw = src.get(k) if isinstance(src.get(k), dict) else {}
        row: dict[str, Any] = {}
        for f in s["fields"]:
            v = raw.get(f["k"], f.get("default", [] if f["type"] == "multi" else ""))
            if f["type"] == "multi":
                v = [str(x) for x in v] if isinstance(v, (list, tuple)) else []
            elif f["type"] == "int":
                try:
                    v = int(v) if str(v).strip() != "" else ""
                except (TypeError, ValueError):
                    v = ""
            else:
                v = "" if v is None else str(v)
            row[f["k"]] = v
        out[k] = row
    return out


def load_draft(conn) -> dict[str, Any]:
    raw = emsdb.get_meta(conn, DRAFT_KEY)
    d = _blank()
    if not raw:
        return d
    try:
        got = json.loads(raw)
    except (TypeError, ValueError):
        # 存坏了就退回空白草稿，不抛 —— 一个坏的 JSON 不该让整个页面打不开。
        return d
    if not isinstance(got, dict):
        return d
    d["steps"] = _clean(got.get("steps"))
    try:
        cs = int(got.get("current_step") or 1)
    except (TypeError, ValueError):
        cs = 1
    d["current_step"] = max(1, min(len(STEPS), cs))
    d["completed"] = bool(got.get("completed"))
    d["completed_at"] = str(got.get("completed_at") or "")
    d["updated_at"] = str(got.get("updated_at") or "")
    d["username"] = str(got.get("username") or "")
    return d


def save_draft(conn, steps: Any, current_step: Any, username: str,
               completed: Optional[bool] = None) -> dict[str, Any]:
    d = load_draft(conn)
    d["steps"] = _clean(steps)
    try:
        cs = int(current_step or d["current_step"] or 1)
    except (TypeError, ValueError):
        cs = 1
    d["current_step"] = max(1, min(len(STEPS), cs))
    d["updated_at"] = _now()
    d["username"] = username or ""
    if completed is True:
        d["completed"] = True
        if not d["completed_at"]:
            d["completed_at"] = d["updated_at"]
    elif completed is False:
        d["completed"] = False
        d["completed_at"] = ""
    emsdb.set_meta(conn, DRAFT_KEY, json.dumps(d, ensure_ascii=False))
    conn.commit()
    return d


def clear_draft(conn) -> None:
    conn.execute("DELETE FROM meta WHERE key = ?", (DRAFT_KEY,))
    conn.commit()


# ---------------------------------------------------------------------
# 校验：缺项清单
# ---------------------------------------------------------------------
def _filled(f: dict[str, Any], v: Any) -> bool:
    if f["type"] == "multi":
        return bool(v)
    return str(v if v is not None else "").strip() != ""


def _dep_value(dep_key: str, row: dict[str, Any],
               allrows: Optional[dict[str, dict[str, Any]]] = None) -> str:
    """依赖项的值：先在本步找，再跨步找。

    ★ 必须支持跨步：第 4 步的「通道 B 是否通」依赖第 1 步的「通道数」。
      只查本步会让这条条件必填永远不生效 —— 而它恰好是双通道站最容易漏的一项。
    """
    if dep_key in row:
        return str(row.get(dep_key, ""))
    for r in (allrows or {}).values():
        if dep_key in r:
            return str(r.get(dep_key, ""))
    return ""


def _required(f: dict[str, Any], row: dict[str, Any],
              allrows: Optional[dict[str, dict[str, Any]]] = None) -> bool:
    """条件必填：dep 里那个字段的取值落在 in 里时才必填。"""
    if not f.get("req"):
        return False
    dep = f.get("dep")
    if dep:
        v = _dep_value(dep["k"], row, allrows)
        return v in [str(x) for x in dep.get("in") or []]
    return True


def missing(steps: Any) -> list[dict[str, Any]]:
    """返回 [{step, step_title, field, label}]，按步骤顺序。空列表 = 可以完成。"""
    out: list[dict[str, Any]] = []
    cleaned = _clean(steps)
    for s in STEPS:
        row = cleaned[s["key"]]
        for f in s["fields"]:
            if not _required(f, row, cleaned):
                continue
            if not _filled(f, row.get(f["k"])):
                out.append({"step": s["num"], "step_key": s["key"],
                            "step_title": s["title"], "field": f["k"],
                            "label": f["label"]})
    return out


def progress(steps: Any) -> dict[str, Any]:
    """每步已填必填数 / 必填总数 —— 步骤条上的点靠它画。"""
    cleaned = _clean(steps)
    per: list[dict[str, Any]] = []
    tot = done = 0
    for s in STEPS:
        row = cleaned[s["key"]]
        req = [f for f in s["fields"] if _required(f, row, cleaned)]
        ok = [f for f in req if _filled(f, row.get(f["k"]))]
        tot += len(req)
        done += len(ok)
        per.append({"step": s["num"], "required": len(req), "filled": len(ok),
                    "ok": len(req) == len(ok) and len(req) > 0})
    return {"required": tot, "filled": done, "steps": per,
            "ready": tot > 0 and done == tot}


# ---------------------------------------------------------------------
# 导出
# ---------------------------------------------------------------------
def _esc(v: Any) -> str:
    """CSV 转义。所有导出共用这一份 —— 两处各写一遍，迟早有一处忘了引号。"""
    s = str(v if v is not None else "")
    if any(c in s for c in (",", '"', "\n", "\r")):
        return '"' + s.replace('"', '""') + '"'
    return s


def _line(cells) -> str:
    return ",".join(_esc(x) for x in cells)


def to_csv(d: dict[str, Any]) -> str:
    """台账 CSV —— 一行一个字段，带必填与状态。

    ★ 为什么按「字段」展开而不是按「步骤」一行：缺项是字段级的，
      一行一步的格式在 Excel 里看不出到底缺哪个格。
    ★ 转发表不并进这份：它是另一份文件（GET /api/upstream/point-map.csv），
      两份东西混成一份，将来谁改了哪份说不清。
    """
    cleaned = _clean(d.get("steps"))
    lines: list[str] = []
    lines.append(",".join(_esc(x) for x in [
        "接入调度主站资料台账", "", "", "", ""]))
    lines.append(",".join(_esc(x) for x in [
        "导出时间", d.get("updated_at") or _now(), "", "", ""]))
    lines.append(",".join(_esc(x) for x in [
        "最后填写人", d.get("username") or "-", "", "", ""]))
    lines.append(",".join(_esc(x) for x in [
        "填报状态", "已完成" if d.get("completed") else "未完成",
        d.get("completed_at") or "", "", ""]))
    lines.append("")
    lines.append(",".join(_esc(x) for x in [
        "步骤", "环节", "项目", "填写内容", "必填", "状态"]))

    for s in STEPS:
        row = cleaned[s["key"]]
        for f in s["fields"]:
            v = row.get(f["k"])
            if f["type"] == "multi":
                mp = {str(a): b for a, b in f.get("opts") or []}
                v = " / ".join(mp.get(str(x), str(x)) for x in (v or []))
            elif f["type"] == "select" or f["type"] == "yesno":
                mp = {str(a): b for a, b in f.get("opts") or []}
                if f["type"] == "yesno":
                    mp = {"yes": "是", "no": "否"}
                v = mp.get(str(v), str(v or ""))
            req = _required(f, row, cleaned)
            ok = _filled(f, row.get(f["k"]))
            lines.append(",".join(_esc(x) for x in [
                s["num"], s["title"], f["label"], v,
                "是" if req else "",
                ("已填" if ok else "缺") if req else ("已填" if ok else "")]))
    lines.append("")
    m = missing(cleaned)
    lines.append(",".join(_esc(x) for x in [
        "缺项合计", len(m), "（必填未填）" if m else "无", "", "", ""]))
    for x in m:
        lines.append(",".join(_esc(y) for y in [
            x["step"], x["step_title"], x["label"], "", "", "缺"]))
    return "\n".join(lines) + "\n"


# =====================================================================
# 每步的「材料清单 / 现状 / 辅助填写 / 本步导出」（v1.15）
#
# 为什么要有这一层：
#   原来九步每步只有几个输入框，第 6 步有两块真配置，其余八步空得像问卷。
#   现场真正要的是「这一步要交什么、按什么口径、现在到底有没有」——
#   那三件事都写在规程里，不是用户心里的默会知识。
#
# ★ 三样东西都放后端，不放前端：
#     · mats  材料清单（含依据与"现状"，现状里的数字从库里真读）
#     · assist 可一键采用的建议值（每句都要能说出为什么）
#     · export 本步该导出的东西（列顺序就是对方要看的顺序）
#   前端只负责画。写死一份在前端，加一行就要改两处。
#
# ★ 「现状」里的占位符由 api.py 传进来的 ctx 填：{port} {ca} {up} {down}
#   {today} {poll} …。填不上的显示「—」，不猜。
#
# ★ assist 只给**有客观依据**的值：
#     · 双通道（并网调度协议普遍要求主备）
#     · 报送日期 = 今天（导出当天的日期没有别的候选）
#     · 审定后点数 = 转发表实际行数（数得出来的不让人手抄）
#   绝不给「结果=通过」这类结论 —— 那是替人签字。
# =====================================================================

DOCS: dict[str, dict[str, Any]] = {
    "1": {
        "mats": {
            "title": "这一步要定下来的四件事",
            # ★ 第四列原来叫「已存的值」，填的是 {dispatch} 这类草稿占位符 ——
            #   草稿要等 0.5 s 自动保存、再翻回这一页才刷新，所以整列常年是「-」。
            #   一整列空着的格子比没有这一列更糟：人会以为软件没存住他刚敲的字。
            #   换成「找谁确认」—— 这一步四件事本来就都要别人点头，
            #   这一列不滞后，而且填错了知道该回去找谁。
            "cols": ["项目", "怎么定", "定错了会怎样", "找谁确认"],
            "rows": [
                ["归口调度", "以接入意见书 / 并网调度协议上的单位名为准",
                 "报错门 ⇒ 整套资料退回重报", "调度自动化专责（要书面回执，口头不算）"],
                ["是否另有上级调管", "分别问清「监视归谁」「AGC/AVC 归谁」",
                 "储能常见「地调监视 + 省调 AGC/AVC」，漏判 = 少一套链路、一套点表",
                 "地调、省调两边的自动化专责都要问一遍"],
                ["通道数", "一条通道 = 一套 2404 监听",
                 "双通道要主备两条；本软件只有一条监听口，第二条得靠外部",
                 "调度通信专责（通道是通信专业开的，不是自动化）"],
                ["接入方式", "专线，或调度数据网（要申请 IP / VLAN）",
                 "数据网申请周期以周计，晚一周就压后面全部工序",
                 "调度数据网管理单位（IP / VLAN 申请单在他们手上）"],
            ],
        },
        "tips": [
            "这一页填错不是「改一格」—— 后面八步全部要重做。",
            "四件事都要对方**书面**点头；口头答应过的，换个人就不认了，这是接入返工最常见的原因。",
            "双通道写进协议 ≠ 软件能双通道：现在只有一个从站实例，第二条通道要靠外部设备或第二个实例。",
        ],
        "assist": [
            {"k": "channels", "v": "双通道",
             "why": "储能并网普遍要求主备双通道（并网调度协议 5.x）。软件侧做不到的部分要单独说明，别默认勾上就算完。"},
        ],
        "export": {"label": "导出 接入方案确认单", "name": "接入方案确认单"},
    },
    "2": {
        "mats": {
            "title": "报送件清单（DL/T 516-2017 7.2 × 并网调度协议 5.5）",
            "cols": ["报送件", "口径 / 依据", "谁提供", "现状"],
            "rows": [
                ["远动信息参数表（点表）", "IOA / 名称 / 类型 / 单位 / 系数，7.2 第 1 项",
                 "我方", "转发表里已有 {up} 上送 + {down} 下行，可直接导出"],
                ["电流互感器 CT 变比", "遥测换算的一次侧依据，协议 5.5 点名要求", "现场 / 设计院",
                 "本软件没有这一列 —— 导出件留空，现场补"],
                ["电压互感器 PT 变比", "同上", "现场 / 设计院", "留空待补"],
                ["遥测满度值", "满度对应 20 mA 或额定值，决定「数准不准」", "现场", "留空待补"],
                ["遥信动合 / 动断", "每个开入的接法，反了就是 0/1 反", "现场", "留空待补"],
                ["扫描周期", "采集侧多长时间读一次", "我方", "{poll} ms（采集器实参）"],
                ["越阈值（死区）", "遥测变化多少才上送", "我方", "未进表，网关侧参数"],
                ["抗抖动滤波时间", "遥信防抖，短了抖、长了丢", "我方", "未进表"],
                ["SOE 设定", "时标分辨率 1 ms，源头打标", "我方", "已具备（时标随 ASDU 上送）"],
                ["通道参数", "IP / 端口 / 公共地址 CA", "我方", "监听 {port} · CA {ca}"],
            ],
        },
        "tips": [
            "「点表报过去就能用」是误解：变比与满度不定，调度看到的数就是错的，而且**不会报错**。",
            "下面那个导出键不只是台账 —— 它把转发表变成《远动信息参数表》，并单独留出「变比 / 满度」两列，空着就是提醒。",
            "软件没有的列不要在这里编：导出件里空着，现场补完再报，比填个 1 更像真数据。",
        ],
        "assist": [
            {"k": "date", "v": "{today}", "why": "报送日期默认取今天；改版重报时改成最后一次报送那天。"},
        ],
        "export": {"label": "导出 远动信息参数表", "name": "远动信息参数表"},
    },
    "3": {
        "mats": {
            "title": "号段与审改：数字都在转发表里，不用手抄",
            # ★ 末列原来是 {segments} / {rounds} 这类草稿回显，要等自动保存 + 重开
            #   才更新 ⇒ 打开就是「-」。改成「本平台能说什么 / 不能说什么」：
            #   号段是对方定的，我们只能报现有点数，这个边界写清楚比回显更有用。
            "cols": ["项目", "口径", "本平台这边"],
            "rows": [
                ["下发号段", "调度按站分配 IOA 号段，我们只做映射",
                 "在对方下发的转发表上；本平台不产生号段，只按它改"],
                ["审定后上送点数", "以调度审定表为准（通常少于我们报的）",
                 "现报 {up} 点（转发表实数，不是估计）"],
                ["审定后下行点数", "遥控 / 遥调点的数量",
                 "现报 {down} 点（同上）"],
                ["改版轮次", "改 2~3 轮是常态，不是异常",
                 "本平台不计数；轮次记在第 9 步的备注里，改一次记一次"],
            ],
        },
        "tips": [
            "号段是对方定的：调度若给 16385 这类号段，本平台的 IOA 号段规则（ioa // 1000）会直接拒存 —— 那时要先改规则，不是硬填。",
            "对方给的号段一旦落进转发表，调度那边的解码就跟着变 —— 所以每改一版都要重新做第 7 步的静态校核。",
            "本步导出的是「号段对照表」：每一行给我们的 IOA、EMS 点名、按现行规则推出的号段，给对点用。",
        ],
        "assist": [
            {"k": "up_count", "v": "{up}", "why": "转发表当前实际上送点数，数得出来的不让人手抄。"},
            {"k": "down_count", "v": "{down}", "why": "转发表当前实际下行点数。"},
        ],
        "export": {"label": "导出 号段对照表", "name": "号段对照表"},
    },
    "4": {
        "mats": {
            "title": "通道路径上要打通的三段",
            # 前两段的地址 / VLAN 只有对方知道，本平台唯一的真状态是「监听在哪」。
            # 回显草稿（{vlan}）反而会让人以为那是配好的值。
            "cols": ["段", "要点", "本平台这边的真状态"],
            "rows": [
                ["调度侧 → 数据网", "主站 IP 固定，我们在它的白名单里",
                 "主站地址只是记下来，本平台不校验它通不通 —— 要通了才知道"],
                ["数据网 → 站内", "VLAN / 路由 / 防火墙策略，双向都要开",
                 "本平台看不到网络侧配置；VLAN 只能靠网络那边给"],
                ["站内 → 本机", "本机网卡、监听口、CA",
                 "监听 {bind}:{port} · CA {ca}（网关现在真的在这个口上等）"],
            ],
        },
        "tips": [
            "104 里**我们是被连的一方**：连不上先查「主站能不能路由到本机」，不要先去怀疑握手参数。",
            "本机监听 0.0.0.0 时，双网卡机器会把两个网段都听着 —— 现场分区要求下必须填具体网卡 IP。",
            "双通道就是 A/B 两套独立路径；软件侧只有一条监听，「通道 B 已通」只能靠外部设备验证。",
        ],
        "assist": [
            {"k": "port", "v": "{port}", "why": "取当前生效的监听口（active.upstream），不是别的机器上的 2404。"},
        ],
        "export": {"label": "导出 通道参数表", "name": "通道参数表"},
    },
    "5": {
        "mats": {
            "title": "证书与安全：全是日历时间，晚一天就卡住",
            # 这一列原来回显 {cert_exp} 等草稿 ⇒ 打开全是「-」，而这一步恰恰最怕
            # 「以为填过了」。改成「谁签发 / 卡住的后果」，这四行每行都能当场去问。
            "cols": ["项目", "要点", "谁签发 · 卡住的后果"],
            "rows": [
                ["纵向加密认证装置", "数据网接入的必备设备，型号要与调度侧的对接台账一致",
                 "调控中心配发的成套设备；型号对不上 = 对方不认这条通道"],
                ["证书申请 → 下发", "向调控中心申请，1~2 周起步，集中报批更久",
                 "调控中心签发；本平台**不能生成**证书，只能记时间"],
                ["证书到期日", "到期前要续，过期当天链路直接不通",
                 "到期当刻就断，且不会提前告警 —— 这是这条链路上唯一会自己失效的东西"],
                ["安全评估", "等保 / 电力监控系统安全防护评估",
                 "有资质的安全测评机构；报告编号要能对上，验收时要查"],
            ],
        },
        "tips": [
            "证书不是软件里能生成的：它由调度侧签发，我们只填「什么时候申请、什么时候到期」。",
            "本平台没有日历提醒 —— 到期日要写进第 9 步的投运资料，让人提前一个月去续。",
            "把到期日填准，比填一堆设备型号有用 —— 它是这条链路唯一会自动到点失效的东西。",
        ],
        "assist": [],
        "export": {"label": "导出 证书与安全清单", "name": "证书与安全清单"},
    },
    "6": {
        "mats": {
            "title": "这一步写的是真配置",
            "cols": ["落地文件", "内容", "谁读它", "现状"],
            "rows": [
                ["config/point-map/active.upstream", "监听网卡 / 监听端口（2404）",
                 "P3 网关（下次启动）", "{upstream}"],
                ["（同上）公共地址 CA", "必须与调度转发表上的站地址一致，对不上回 UNKNOWN_CA",
                 "P3 网关（下次启动）", "CA {ca}"],
                ["config/point-map/active.pointmap.csv", "IOA 转发表 {up} 上送 + {down} 下行",
                 "P3 网关（下次启动）", "{pmfile}"],
            ],
        },
        "tips": [
            "★ 这张表是**本机**的对照表，不是交给调度的文件 —— 调度那份叫《远动信息参数表》，第 2 步导出。"
            "但里面的 IOA 号**是调度定的**：现在这一版是项目自定的建议号段，没经调度确认。",
            "目录里还有个 config/point-map/active.csv，那是**下端 Modbus 点表**，和这张表只差一个点号，"
            "手工改文件时别改错 —— 改错会把下端采集打坏。",
            "改完要重启网关才生效（暂不做热更新）—— 保存成功的提示里会写清这一点。",
            "用模拟主站跑通只证明「我们这边能对上」，不证明调度那边通了。",
        ],
        "assist": [],
        "export": {"label": "导出 站内配置快照", "name": "站内配置快照"},
    },
    "7": {
        "mats": {
            "title": "三级联调（行业固定三步法）",
            # 前三行是行业三步法，后两行是本项目踩出来的：这两项不测，
            # 前面三步全绿也照样在投运当晚出事。
            "cols": ["阶段", "查什么", "怎么算过"],
            "rows": [
                ["静态校核", "点对点核对：IOA 对上了，绑的内部点是不是也对", "逐点表格双方签认"],
                ["闭环测试", "模拟主站发总召 / 变化，看数值、品质、时标", "与源端逐点一致"],
                ["动态校核", "真实工况下看越限、变位、SOE 时序", "无错序、无丢失"],
                ["总召一致性", "主站发总召（QOI=20），24 点上送必须全回来",
                 "★ 本网关只认 QOI=20 站总召；QOI 21~36 分组总召回负确认"],
                ["通道中断重连", "拔网线 / 重启主站，看能不能自己恢复", "STARTDT 重发成功，数据自动续上"],
            ],
        },
        "tips": [
            "静态校核专抓「拖错点」：IOA 配对、绑的内部点错了 —— 表看着全对，数全是别人的。",
            "结论只写「通过 / 不通过」，不写「基本通过」；遗留项数量必须落一个数。",
            "总召那一项最容易漏：点表对着、变化也上送，可主站一做总召就少点 —— 分组总召本网关直接拒，要在联调时当面说清。",
        ],
        "assist": [],
        "export": {"label": "导出 联调记录表", "name": "联调记录表"},
    },
    "8": {
        "mats": {
            "title": "传动：每个下行点都要真动一次",
            # 第一列末格原来是「同上」，等于没说 —— 每一行都说清自己怎么算数完。
            "cols": ["对象", "怎么做", "怎么算完"],
            "rows": [
                ["遥控点", "选择 → 执行 → 返校，看开关真动",
                 "本表下行 {down} 点逐个来；返校正确 + 位置变位 + SOE 有记录，三样齐"],
                ["遥调点", "给定 → 看功率 / 电压真变化",
                 "给定值到场后读数跟上，且不越 L0 安全边界"],
                ["闭锁试验", "就地 / 检修状态下命令应被拒",
                 "要有拒绝记录，不是「没动」—— 没反应也可能是命令根本没送到"],
            ],
        },
        "tips": [
            "「命令下发成功」不等于「开关动了」：返校正确、位置变位、SOE 有记录，三样齐了才算传过。",
            "本步导出逐点一行（下行点全列出来），结果列照你填的写，没填就是空 —— 空着比写「通过」诚实。",
        ],
        "assist": [
            {"k": "ctl_point", "v": "{down_first}", "why": "转发表里第一个下行点，作为遥控传动的默认对象。"},
        ],
        "export": {"label": "导出 传动试验记录", "name": "传动试验记录"},
    },
    "9": {
        "mats": {
            "title": "投运：签字、试运行、归档",
            # 两列太薄，补一列「交给谁 / 凭据」—— 这一步的难点全在"东西交到谁手上"。
            "cols": ["项目", "要点", "交给谁 · 凭据"],
            "rows": [
                ["对点报告", "双方签字是拿并网调度协议的前置条件之一",
                 "正本交调控中心自动化处；我方留盖章件"],
                ["试运行", "连续运行时长与考核指标按协议约定",
                 "运行值班出考核记录，交调度；不是自己写一段话"],
                ["归档", "点表、联调记录、传动记录、报告编号要能对上",
                 "站内资料柜 + 本平台导出件；编号三处必须一致"],
                ["证书与安全", "证书到期日提前一个月去续；等保报告要有编号",
                 "写进本清单，让人到点能看见（本平台没有日历提醒）"],
                ["遗留项", "缺项清单要能逐条解释「为什么空」",
                 "本平台「九步总台账」末尾的缺项合计；验收时要对着念"],
            ],
        },
        "tips": [
            "归档位置写「哪个盘 / 哪个柜 / 哪个台账号」，别写「已归档」—— 半年后没人找得到。",
            "本步导出的是投运资料清单 + 九步汇总：一页纸看清哪一步是谁签的。",
        ],
        "assist": [],
        "export": {"label": "导出 投运资料清单", "name": "投运资料清单"},
    },
}


def _step(num: Any) -> dict[str, Any]:
    for s in STEPS:
        if str(s["num"]) == str(num):
            return s
    raise KeyError(f"没有第 {num} 步")


def _ctx_or_blank(ctx: Optional[dict[str, Any]]) -> dict[str, Any]:
    """把缺的占位符补成「—」：宁可显示没填，也不要让 {port} 这种字面量跑到界面上。"""
    d = dict(ctx or {})
    d.setdefault("today", "")
    for k in ("port", "ca", "bind", "up", "down", "poll", "up_first", "down_first"):
        d.setdefault(k, "")
    return d


def _fill(text: Any, ctx: dict[str, Any]) -> str:
    s = str(text if text is not None else "")
    out = []
    i = 0
    while True:
        j = s.find("{", i)
        if j < 0:
            out.append(s[i:])
            break
        k = s.find("}", j)
        if k < 0:
            out.append(s[i:])
            break
        out.append(s[i:j])
        key = s[j + 1:k]
        v = ctx.get(key)
        out.append("-" if v in (None, "") else str(v))
        i = k + 1
    return "".join(out)


def detail(num: Any, ctx: Optional[dict[str, Any]] = None) -> dict[str, Any]:
    """本步的材料清单与提示（占位符已按 ctx 填好）。"""
    c = _ctx_or_blank(ctx)
    d = DOCS.get(str(_step(num)["num"]), {})
    mats = d.get("mats") or {}
    return {
        "mats": {
            "title": mats.get("title") or "",
            "cols": list(mats.get("cols") or []),
            "rows": [[_fill(x, c) for x in r] for r in (mats.get("rows") or [])],
        } if mats else None,
        "tips": [_fill(t, c) for t in (d.get("tips") or [])],
    }


def assist(num: Any, ctx: Optional[dict[str, Any]] = None) -> list[dict[str, str]]:
    """可一键采用的建议值。只给有客观依据的，不给结论。"""
    c = _ctx_or_blank(ctx)
    d = DOCS.get(str(_step(num)["num"]), {})
    out = []
    for a in d.get("assist") or []:
        v = _fill(a["v"], c)
        if v == "-":          # 依据本身缺失（比如转发表还没落盘）⇒ 不给建议
            continue
        out.append({"k": a["k"], "v": v, "why": a["why"]})
    return out


def export_spec(num: Any) -> dict[str, str]:
    d = DOCS.get(str(_step(num)["num"]), {})
    e = d.get("export") or {}
    return {"label": e.get("label") or "导出本步", "name": e.get("name") or "接入资料"}


def _field_map(s: dict[str, Any], row: dict[str, Any]) -> list[list[str]]:
    """本步字段 → 台账行。选项值翻成人话。"""
    out = []
    for f in s["fields"]:
        v = row.get(f["k"])
        if f["type"] == "multi":
            mp = {str(a): b for a, b in f.get("opts") or []}
            v = " / ".join(mp.get(str(x), str(x)) for x in (v or []))
        elif f["type"] == "select":
            v = {str(a): b for a, b in f.get("opts") or []}.get(str(v), str(v or ""))
        elif f["type"] == "yesno":
            v = {"yes": "是", "no": "否"}.get(str(v), str(v or ""))
        out.append([f["label"], str(v or ""), "是" if f.get("req") else ""])
    return out


def _pm(ctx: dict[str, Any], down: bool) -> list[dict[str, Any]]:
    rows = ctx.get("pm") or []
    return [r for r in rows if (str(r.get("direction") or "") == "down") == down]


def export_step(num: Any, steps: Any,
                ctx: Optional[dict[str, Any]] = None) -> tuple[str, str]:
    """本步的导出件 —— 返回 (文件名, CSV 文本)。

    ★ 三处不是「台账换格式」，是真的按对方要看的样子拼：
      第 2 步 = 远动信息参数表（逐点 + 变比/满度空列）
      第 3 步 = 号段对照表（我们的 IOA → 按规则推的号段）
      第 8 步 = 传动记录（下行点逐行，结果列照填）
      其余六步 = 本步台账（项目 / 内容 / 必填），末尾带缺项。
    ★ 缺项一律写进文件末尾，不静默：导出件不能看起来比草稿更完整。
    """
    n = int(_step(num)["num"])
    c = _ctx_or_blank(ctx)
    s = _step(n)
    row = (_clean(steps).get(s["key"]) or {})
    spec = export_spec(n)
    L: list[str] = []

    head = [
        f"{spec['name']}",
        f"导出时间,{_now()}",
        f"环节,第 {n} 步 · {s['title']}",
    ]
    if c.get("upstream"):
        head.append(f"上端链路,{c['upstream']}")
    L.extend(head)
    L.append("")

    if n == 2:
        L.append(_line(["报送信息"]))
        for lab, val, req in _field_map(s, row):
            L.append(_line([lab, val, req and "必填" or ""]))
        L.append("")
        L.append(_line(["远动信息参数表（DL/T 516-2017 7.2 口径）"]))
        L.append(_line(["序号", "IOA", "方向", "信息名称", "类型", "EMS 点名",
                        "系数", "偏置", "单位", "CT 变比", "PT 变比",
                        "遥测满度", "遥信动合/动断", "备注"]))
        idx = 0
        for r in (c.get("pm") or []):
            idx += 1
            L.append(_line([
                idx, r.get("ioa", ""),
                "下行" if str(r.get("direction")) == "down" else "上送",
                r.get("name", ""), r.get("kind", ""), r.get("src", ""),
                r.get("scale", ""), r.get("offset", ""), r.get("unit", ""),
                "", "", "", "", "",
            ]))
        L.append("")
        L.append("# 变比 / 满度 / 动合动断 三组列本软件不产生，留给现场按设计值补 ——"
                 " 空着是提醒，不是默认 1。")
    elif n == 3:
        L.append(_line(["审定信息"]))
        for lab, val, req in _field_map(s, row):
            L.append(_line([lab, val, req and "必填" or ""]))
        L.append("")
        L.append(_line(["号段对照表"]))
        L.append(_line(["序号", "我们的 IOA", "方向", "信息名称", "EMS 点名",
                        "按现行规则推出的号段", "是否保留", "备注"]))
        for i, r in enumerate(c.get("pm") or [], 1):
            ioa = r.get("ioa")
            seg = (str(int(ioa) // 1000) + "xxx") if isinstance(ioa, int) else ""
            L.append(_line([i, ioa, "下行" if str(r.get("direction")) == "down" else "上送",
                            r.get("name", ""), r.get("src", ""), seg, "", ""]))
        L.append("")
        L.append("# 号段是调度分配的：调度给的号段若不在 1xxx/4xxx/5xxx/6xxx/8xxx 里，"
                 "平台的号段规则会拒存，要先改规则。")
    elif n == 8:
        L.append(_line(["传动结论"]))
        for lab, val, req in _field_map(s, row):
            L.append(_line([lab, val, req and "必填" or ""]))
        L.append("")
        L.append(_line(["传动记录（逐点）"]))
        L.append(_line(["序号", "IOA", "对象", "EMS 点名", "遥控结果",
                        "遥调结果", "返校", "SOE 有记录", "备注"]))
        for i, r in enumerate(_pm(c, down=True), 1):
            L.append(_line([i, r.get("ioa", ""), r.get("name", ""), r.get("src", ""),
                            "", "", "", "", ""]))
        L.append("")
        L.append("# 「命令下发成功」不等于「开关动了」：返校、位置变位、SOE 三样齐了才算传过。")
    else:
        for lab, val, req in _field_map(s, row):
            L.append(_line([lab, val, req and "是" if req else ""]))
        if n == 9:
            L.append("")
            L.append(_line(["九步汇总"]))
            L.append(_line(["步骤", "环节", "必填", "已填", "状态"]))
            pg = progress(steps)
            for st, p in zip(STEPS, pg["steps"]):
                L.append(_line([st["num"], st["title"], p["required"], p["filled"],
                                "齐" if p["ok"] else "缺 %d 项" % (p["required"] - p["filled"])]))

    # 缺项：一律写在文件末尾 —— 导出件不能看起来比草稿更完整
    miss = [m for m in missing(_clean(steps)) if int(m["step"]) == n]
    L.append("")
    L.append(_line(["缺项合计", len(miss), "（本步必填未填）" if miss else "无"]))
    for m in miss:
        L.append(_line(["缺", m["label"], ""]))
    fname = f"{spec['name']}_第{n}步.csv"
    return fname, "\n".join(L) + "\n"
