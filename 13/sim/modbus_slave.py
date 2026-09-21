#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
13/ — Python Modbus TCP 从站：PCS / BMS / 电表三合一的 **设备替身**

它存在的两个理由（都不是"再写一遍协议"）：

  ① **跨语言联调**：证明 13/src/modbus_tcp_client.h 能和**别人的实现**对上话，
     而不只是能和自己写的 C++ fake 从站对上话。
     ★ 这一条不能省：自己写两端等于自己和自己对答案 —— 双方共同误解协议时
     谁都发现不了。pymodbus 是业界事实标准，拿它当对端才有说服力。
     实践中它立刻就抓出了两个真问题（见文件末尾「联调踩到的坑」）。

  ② **现场预演**：把 --load / --pv / --bias 设成现场工况，观察 EMS 的响应。

与 13/src/fake_modbus_slave.h 的分工不是重复，是**两类不同的保证**：
     fake（进程内）      → 我们的客户端符合协议，且异常路径处理正确
     Python（跨进程）    → 两个独立实现之间能真正互通

---------------------------------------------------------------------
点表一致性
---------------------------------------------------------------------
本文件的点表与 13/src/modbus_point_map.h **必须逐点一致**。两边都是手工维护的，
任何一边改动都要同步另一边；`--self-check` 会打印完整对照表，用来人工核对。

为什么不做成"自动生成"：现场点表是**设备厂家给的**，它先于我们的代码存在。
把点表做成"从代码生成"会掩盖"现场点表和我们以为的不一样"这个真问题。

---------------------------------------------------------------------
与 pymodbus 版本有关的硬事实（3.15 实测，不是读文档看出来的）
---------------------------------------------------------------------
① **不要走 `ModbusDeviceContext` + `ModbusSequentialDataBlock` 老路。**
   它在 3.15 里已被标记弃用，更要命的是 `ModbusDeviceContext.__init__` 会对
   传入的 data block 做 `deepcopy` —— 构造之后再改 `ir.simdata[0].values[...]`
   **完全不影响服务端**，而且**不报错**。表现是"主站永远读到 0"。
   正确做法：直接用 `pymodbus.simulator` 的 `SimData/SimDevice` +
   `pymodbus.server.ModbusTcpServer`（非弃用路径）。

② **`SimData(0, 1, values=[...]*64, datatype=REGISTERS|BITS)`**
   · 地址 0 == 协议地址 0，与 `modbus_point_map.h` 的 0 基地址**直接对齐**；
   · `count` 必须传 **1**：REGISTERS 会忽略它，但 BITS 会**乘**开
     （实测 count=64 + 64 个 bool → 展开成 4096 位）；
   · `SimDevice(unit, simdata=(co, di, hr, ir))` 四元组顺序**固定**是
     线圈/离散输入/保持/输入 —— 写反了四张表整体错位，而四张表地址都从 0 起，
     错位后照样读写成功，只是读到别的区的值。**只能靠跨语言对账抓。**

③ **更新寄存器只有一个公开入口**：`server.async_setValues(unit, fc, addr, values)`，
   其中 `fc` 是**标准 Modbus 功能码**（1=CO 2=DI 3=HR 4=IR），不是表名。
   读回用 `server.async_getValues(unit, fc, addr, qty)`。
   所以脚本必须**自己持有 `ModbusTcpServer` 实例**
   （`StartAsyncTcpServer()` 是"fire and forget"，拿不到句柄）。

用法：
    python modbus_slave.py --port 15020
    python modbus_slave.py --port 15020 --load 380 --pv 150 --bias 40 --verbose
    python modbus_slave.py --self-check
    python modbus_slave.py --dump-tsv      # 供跨语言一致性比对
"""

import argparse
import asyncio
import math
import struct
import sys
import time

try:
    # ★ 用**非弃用**路径：SimData / SimDevice / ModbusTcpServer。
    #   pymodbus 3.15 里 `ModbusDeviceContext` + `ModbusSequentialDataBlock` 那条老路
    #   已经废弃，而且**有个致命行为**：`ModbusDeviceContext.__init__` 对传入的
    #   data block 做 `deepcopy`，构造之后再改 `ir.simdata[0].values[...]`
    #   根本不会影响服务端 —— 老路在 3.15 里已经退化成"一次性初始化器"。
    #   更关键的是：老路**没有任何公开的 API 能从外部更新寄存器**，
    #   而我们需要模型每拍刷新 32 个点。
    from pymodbus.simulator import SimData, SimDevice, DataType
    from pymodbus.server import ModbusTcpServer
    HAVE_PYMODBUS = True
    PYMODBUS_ERROR = ""
except ImportError as e:  # pragma: no cover - 环境缺失时给出可执行的指引
    # ★ **不在这里退出**：`--dump-tsv` / `--self-check` 是**点表比对**，
    #   它们不应该依赖第三方库。真正要起服务时才报缺依赖 ——
    #   否则"环境没装 pymodbus"会被误读成"点表不一致"。
    HAVE_PYMODBUS = False
    PYMODBUS_ERROR = str(e)


def _require_pymodbus():
    if HAVE_PYMODBUS:
        return
    sys.stderr.write(
        "缺少 pymodbus（%s）。请先建立隔离环境：\n"
        "  python -m venv .venv\n"
        "  .venv/Scripts/pip install -r 13/sim/requirements.txt\n"
        % PYMODBUS_ERROR
    )
    raise SystemExit(2)

REG_COUNT = 64  # 四个区的容量（点表用不到这么多，留余量便于现场临时加点）

# =====================================================================
# 点表（**必须与 13/src/modbus_point_map.h 一致**）
# =====================================================================
# ---- 输入寄存器 IR（只读，FC04）----
IR_P_LOAD = 0    # f32 高字在前
IR_P_PV = 2      # f32 高字在前
IR_P_BAT = 4     # f32 **低字在前**（刻意，见 modbus_point_map.h 的说明）
IR_P_GRID = 6    # f32 高字在前
IR_SOC = 8       # u16 ×10000
IR_T_C = 9       # i16 ×10
IR_SOH = 10      # u16 ×10000
IR_CFG_BASE = 12  # 从 12 起，14 个 f32，每点占 2 个寄存器

# 配置项顺序（与映射表逐项对应）
CFG_ORDER = [
    ("CFG.BAT_CAP_KWH", "cap_kwh"),
    ("CFG.PCS_MAX_CHG", "pcs_max_chg"),
    ("CFG.PCS_MAX_DIS", "pcs_max_dis"),
    ("CFG.BMS_CHG_LIM", "bms_chg_lim"),
    ("CFG.BMS_DIS_LIM", "bms_dis_lim"),
    ("CFG.TRANSFORMER_KVA", "tr_kva"),
    ("CFG.D_TARGET", "d_target"),
    ("CFG.TAU_S", "tau_s"),
    ("CFG.PCS_RAMP_KW_PER_S", "ramp_kw_s"),
    ("CFG.PCS_STANDBY", "standby_kw"),
    ("CFG.ETA_CHG", "eta_chg"),
    ("CFG.ETA_DIS", "eta_dis"),
    ("CFG.SOC_PHYS_MIN", "soc_min"),
    ("CFG.SOC_PHYS_MAX", "soc_max"),
]

# ---- 保持寄存器 HR（可读写，FC03/06/16）：EMS 写下来的指令 ----
HR_P_BAT = 0
HR_P_UPPER = 2
HR_P_LOWER = 4

# ---- 离散输入 DI（只读，FC02）：设备状态位 ----
DI_BMS_COMM_OK = 0
DI_PCS_COMM_OK = 1
DI_METER_COMM_OK = 2
DI_PCS_FAULT = 3
DI_OFFLINE = 4
DI_DATA_VALID = 5
DI_BMS_CHG_FORBID = 6   # ★ 安全输入（1 = BMS 禁充）
DI_BMS_DIS_FORBID = 7   # ★ 安全输入（1 = BMS 禁放）

# ---- 功能码（供 server.async_getValues / async_setValues 用）----
# pymodbus 内部按**标准 Modbus 功能码**分派到四张表，不是按表名：
#     1 = 线圈(CO)  2 = 离散输入(DI)  3 = 保持寄存器(HR)  4 = 输入寄存器(IR)
FC_CO = 1
FC_DI = 2
FC_HR = 3
FC_IR = 4


# =====================================================================
# 编码工具（与 modbus_point_map.h 的 put_f32 / kU16 / kI16 一致）
# =====================================================================
def f32_to_regs(value, low_word_first=False):
    """32 位浮点 → 两个寄存器。Modbus 线上恒为大端；字序由映射表逐点声明。"""
    hi, lo = struct.unpack(">HH", struct.pack(">f", float(value)))
    return [lo, hi] if low_word_first else [hi, lo]


def regs_to_f32(regs, low_word_first=False):
    hi, lo = (regs[1], regs[0]) if low_word_first else (regs[0], regs[1])
    return struct.unpack(">f", struct.pack(">HH", hi, lo))[0]


def u16_scaled(value, scale):
    """无符号缩放。越界**不静默饱和** —— 与 C++ 侧 encode_point 同一纪律：
    饱和会把"5000 kW"变成"65535"，设备照做就出事。"""
    raw = int(round(float(value) * scale))
    if raw < 0 or raw > 0xFFFF:
        raise ValueError(f"u16 越界: {value} × {scale} = {raw}")
    return raw


def i16_scaled(value, scale):
    raw = int(round(float(value) * scale))
    if raw < -32768 or raw > 32767:
        raise ValueError(f"i16 越界: {value} × {scale} = {raw}")
    return raw & 0xFFFF


# =====================================================================
# 设备模型
# =====================================================================
class DeviceModel:
    """一个刻意简化的储能设备：一阶惯性执行 + SOC 积分 + 关口电表。

    为什么模型要和 07/src/plant_model.h 同式（一阶惯性 + ramp + 效率积分）：
    只有同式，跨语言联调的结果才能和单进程仿真对照 —— 否则"数值对不上"
    到底是协议错还是模型错就分不清了。
    """

    def __init__(self, args):
        self.args = args
        self.soc = args.soc0
        self.p_bat = 0.0          # 实际功率（放电为正）
        self.t_c = args.ambient_c
        self.p_load = args.load
        self.p_pv = args.pv

        # 配置（设备侧固有参数）
        self.cap_kwh = args.cap_kwh
        self.pcs_max_chg = args.pcs_max_chg
        self.pcs_max_dis = args.pcs_max_dis
        self.bms_chg_lim = args.bms_chg_lim
        self.bms_dis_lim = args.bms_dis_lim
        self.tr_kva = args.tr_kva
        self.d_target = args.d_target
        self.tau_s = args.tau_s
        self.ramp_kw_s = args.ramp_kw_s
        self.standby_kw = args.standby_kw
        self.eta_chg = args.eta_chg
        self.eta_dis = args.eta_dis
        self.soc_min = args.soc_min
        self.soc_max = args.soc_max
        # ★ 电表系统偏差：**故意让电表读数不等于三路平衡值**。
        #   这不是"模型不严谨"，而是必备的**区分度**：只有当两个口径的数值
        #   真的不同，跨语言测试才能证明主站读的是电表寄存器而不是自己相减。
        #   取值也刻意：与 07/ 的 meter_bias_kw 同量级（40 kW）。
        self.meter_bias = args.meter_bias

        self.cmd_received = 0
        self.last_cmd = (0.0, 0.0, 0.0)

    # ---- 从指令区读（HR）----
    # ★ 参数是**寄存器数组**，不是存储对象：DeviceModel 不做 I/O。
    #   模型只回答"给定这 6 个寄存器和 dt，下一个状态是什么" ——
    #   这样它和 07/src/plant_model.h 才能逐行对照，
    #   也让"协议层"与"物理层"的缺陷可以分别定位。
    def read_command(self, hr_regs):
        p_bat = regs_to_f32(hr_regs[HR_P_BAT:HR_P_BAT + 2], low_word_first=False)
        p_up = regs_to_f32(hr_regs[HR_P_UPPER:HR_P_UPPER + 2], low_word_first=False)
        p_lo = regs_to_f32(hr_regs[HR_P_LOWER:HR_P_LOWER + 2], low_word_first=False)
        # NaN 是"EMS 尚未下发"的常见表示（未初始化的保持寄存器读回来常是 0，
        # 但被写坏时会是 NaN）。一律当作 0 区间会**锁死设备**，
        # 所以这里按"未下发"处理（保持上一次的权限区间）。
        if not (math.isfinite(p_bat) and math.isfinite(p_up) and math.isfinite(p_lo)):
            return None
        return (p_bat, p_up, p_lo)

    def step(self, hr_regs, dt):
        cmd = self.read_command(hr_regs)
        if cmd is not None:
            self.last_cmd = cmd
            self.cmd_received += 1
        p_cmd, p_up, p_lo = self.last_cmd

        # ① EMS 下发的权限区间（安全层折出来的结果）
        p_cmd = min(max(p_cmd, p_lo), p_up)
        # ② 设备自身能力
        p_cmd = min(max(p_cmd, -self.pcs_max_chg), self.pcs_max_dis)
        # ③ 变化率
        dmax = max(0.0, self.ramp_kw_s) * dt
        p_cmd = min(max(p_cmd, self.p_bat - dmax), self.p_bat + dmax)
        # ④ 一阶惯性
        tau = max(1e-6, self.tau_s)
        alpha = 1.0 - math.exp(-dt / tau)
        self.p_bat += (p_cmd - self.p_bat) * alpha
        self.p_bat = min(max(self.p_bat, -self.pcs_max_chg), self.pcs_max_dis)

        # ⑤ SOC 积分（含充放电效率）
        if self.p_bat > 0.0:
            d_energy = -self.p_bat * dt / 3600.0 / max(1e-6, self.eta_dis)
        elif self.p_bat < 0.0:
            d_energy = -self.p_bat * dt / 3600.0 * self.eta_chg
        else:
            d_energy = 0.0
        self.soc += d_energy / max(1e-6, self.cap_kwh)
        self.soc = min(max(self.soc, self.soc_min), self.soc_max)

        # ⑥ 关口电表（**唯一权威计量点**）：
        #    = 负荷 + 站用电 - 光伏 - 电池 + 电表系统偏差
        #    与 PlantModel::meter_p_grid() / MemoryDeviceIO::update_grid() 同式。
        #
        # ★ `meter_bias` 必须**真的加进去**。默认 0 时"加了"与"忘了加"
        #   逐位相等 —— 所以默认值证明不了这一行存在。联调时用 --bias 40
        #   把它拉开：主站若在本地重算 (load+standby-pv-p_bat)，会得到比
        #   电表读数小 40 kW 的值，这正是"口径没统一"的可观测差异。
        p_grid = (self.p_load + self.standby_kw) - self.p_pv - self.p_bat \
            + self.meter_bias

        return p_grid

    # ---- 生成要写回的量测（IR）----
    # ★ 返回 `[(FC_IR, addr, values), ...]` 而不是直接写存储：
    #   模型不知道"写"是怎么发生的（TCP 从站？内存？），
    #   这样它才能在不起服务的情况下被单测（与 C++ 侧的解码器共用同一批断言）。
    def measurement_writes(self, p_grid):
        return [
            (FC_IR, IR_P_LOAD, f32_to_regs(self.p_load)),
            (FC_IR, IR_P_PV, f32_to_regs(self.p_pv)),
            (FC_IR, IR_P_BAT, f32_to_regs(self.p_bat, low_word_first=True)),
            (FC_IR, IR_P_GRID, f32_to_regs(p_grid)),
            (FC_IR, IR_SOC, [u16_scaled(self.soc, 10000.0)]),
            (FC_IR, IR_T_C, [i16_scaled(self.t_c, 10.0)]),
            (FC_IR, IR_SOH, [u16_scaled(self.args.soh, 10000.0)]),
        ]

    def config_writes(self):
        vals = {
            "cap_kwh": self.cap_kwh,
            "pcs_max_chg": self.pcs_max_chg,
            "pcs_max_dis": self.pcs_max_dis,
            "bms_chg_lim": self.bms_chg_lim,
            "bms_dis_lim": self.bms_dis_lim,
            "tr_kva": self.tr_kva,
            "d_target": self.d_target,
            "tau_s": self.tau_s,
            "ramp_kw_s": self.ramp_kw_s,
            "standby_kw": self.standby_kw,
            "eta_chg": self.eta_chg,
            "eta_dis": self.eta_dis,
            "soc_min": self.soc_min,
            "soc_max": self.soc_max,
        }
        return [
            (FC_IR, IR_CFG_BASE + i * 2, f32_to_regs(vals[key]))
            for i, (_name, key) in enumerate(CFG_ORDER)
        ]

    # ---- 生成要写回的状态位（DI）----
    def status_writes(self, args):
        # 一次写 8 位（同一段连续地址）—— 与 C++ 侧的 3 次分块读对称：
        # 分块是**契约**，不是实现细节。
        bits = [
            True,                # DI_BMS_COMM_OK
            True,                # DI_PCS_COMM_OK
            True,                # DI_METER_COMM_OK
            bool(args.pcs_fault),   # DI_PCS_FAULT
            bool(args.offline),     # DI_OFFLINE
            True,                # DI_DATA_VALID
            # ★ 两个安全位：现场由 BMS 网关**每拍显式写**。
            #   位类型尤其危险 —— 从站不刷新时它停在 0 = "允许充放"，
            #   与"根本没配这条线"在值上完全不可区分。本脚本用命令行参数
            #   模拟网关上报，并且**每拍都重写**（不是只在启动时写一次）。
            bool(args.bms_chg_forbid),  # DI_BMS_CHG_FORBID
            bool(args.bms_dis_forbid),  # DI_BMS_DIS_FORBID
        ]
        return [(FC_DI, 0, bits)]


# =====================================================================
# 从站存储：**唯一允许碰 pymodbus 的地方**
#
# 为什么把 I/O 收在这一个类里：
#   · 上面 DeviceModel 保持纯函数，能被无服务地单测；
#   · pymodbus 的 API 在 3.x → 4.x 之间反复改名（见文件末尾「联调踩到的坑」），
#     把变动面收在 40 行内，升级时只改这一处。
#
# ★ 关键实测结论（读源码看不出来，跑出来才知道）：
#   `ModbusTcpServer` 的 `async_setValues(unit, fc, addr, values)` 是
#   **唯一**能从外部改到"服务端真正在读的那份存储"的公开入口。
#   老的 `ModbusSequentialDataBlock` 路线在 3.15 里已被 `deepcopy` 断开，
#   改 data block 完全无效 —— 而且**不报错**，表现是"主站永远读到 0"。
# =====================================================================
class SlaveStore:
    """Modbus TCP 从站的存储与服务。

    `async_setValues` 的功能码语义（pymodbus 按标准 FC 分派，不按表名）：
        1 = 线圈(CO)  2 = 离散输入(DI)  3 = 保持寄存器(HR)  4 = 输入寄存器(IR)
    """

    def __init__(self, host, port, unit_id):
        # ★ SimData 的地址语义：`SimData(0, ...)` == 协议地址 0。
        #   与 C++ 侧 modbus_point_map.h 的 0 基地址**直接对齐**，无需 ±1。
        #   ★ `count` 必须传 1：REGISTERS 忽略它，但 BITS 会**乘**开
        #     （实测 count=64 + 64 个 bool 会展开成 4096 位）。
        co = [SimData(0, 1, [False] * REG_COUNT, DataType.BITS)]
        di = [SimData(0, 1, [False] * REG_COUNT, DataType.BITS)]
        hr = [SimData(0, 1, [0] * REG_COUNT, DataType.REGISTERS)]
        ir = [SimData(0, 1, [0] * REG_COUNT, DataType.REGISTERS)]
        # 四元组顺序固定是 (coils, discrete, holding, input)，对应
        # SimDevice.build_device() 里的 convert = {0:"c",1:"d",2:"h",3:"i"}。
        #
        # ★ 关于"写反了会怎样"——**两条实测结论，别照抄直觉**：
        #   ① **跨类型**写反（BITS 放到 REGISTERS 位，或反之）：
        #      pymodbus 在构造时就拒绝，报
        #      `TypeError: simdata[0]=tuple[discrete inputs] -> list[0] not
        #       DataType.BITS, not allowed`。**不会静默**。
        #   ② **同类型**写反（CO↔DI 或 HR↔IR，两边数据类型相同）：
        #      类型检查挡不住 —— 但**在本文件里也没有影响**。
        #      因为表身份由**位置**决定，而我们对每一张表的读和写
        #      **都走同一个功能码入口**（FC 绑定位置，不绑定对象），
        #      所以把哪个 SimData 对象放在第 1 位并不改变对外语义。
        #      （实测：交换 CO/DI 后 13/tests/test_modbus_bridge.cpp 仍 83/0 全绿。）
        #
        #   所以真正的风险不在**元组顺序**，而在 **C++ 侧 Table 枚举与标准
        #   功能码的对应**写错 —— 那才会"读错表"，且协议层完全合法。
        #   那一条由 T40（点表逐字段比对）+ T41（32 点值比对）守着。
        self.device = SimDevice(unit_id, simdata=(co, di, hr, ir))
        self.server = ModbusTcpServer(self.device, address=(host, port))
        self.unit = unit_id
        self.writes = 0

    async def serve(self):
        await self.server.serve_forever()

    async def read_block(self, fc, addr, qty):
        return await self.server.async_getValues(self.unit, fc, addr, qty)

    async def write_block(self, fc, addr, values):
        await self.server.async_setValues(self.unit, fc, addr, values)
        self.writes += 1

    async def write_many(self, writes):
        for fc, addr, values in writes:
            await self.write_block(fc, addr, values)


async def model_loop(store, model, args):
    """设备模型循环。

    为什么跑在 asyncio 里而不是另一个线程：
      · pymodbus 的存储没有跨线程同步；
      · 更硬的理由：`await` 的**单线程顺序**天然保证"读指令 → 算状态 →
        写量测"这一串是原子的，主站不会读到"新功率 + 旧 SOC"这种
        半拍拼接的状态。
      这与 RT_DB 那边要靠 seqlock 解决的问题是同一个，只是这里用
      "单线程顺序"简单绕开了 —— 代价是模型算得慢会拖慢服务，
      所以脚本里把模型做得极轻，并把 sleep 放到步末。
    """
    dt = args.dt
    next_t = time.monotonic()
    ticks = 0
    while True:
        # ① 读 EMS 写下来的指令（HR 区，6 个寄存器 = 3 个 f32）
        hr_regs = await store.read_block(FC_HR, 0, 6)
        # ② 推进物理状态
        p_grid = model.step(hr_regs, dt)
        # ③ 写回量测与状态
        await store.write_many(model.measurement_writes(p_grid))
        await store.write_many(model.status_writes(args))
        ticks += 1

        if args.verbose and ticks % max(1, int(1.0 / max(1e-3, dt))) == 0:
            print(
                f"  [t={ticks * dt:7.1f}s] p_bat={model.p_bat:8.2f} "
                f"p_grid={p_grid:8.2f} soc={model.soc:.4f} "
                f"cmd=({model.last_cmd[0]:.1f},{model.last_cmd[1]:.1f},"
                f"{model.last_cmd[2]:.1f}) ncmd={model.cmd_received}",
                flush=True,
            )

        if args.duration > 0 and ticks * dt >= args.duration:
            return

        next_t += dt
        delay = next_t - time.monotonic()
        if delay > 0:
            await asyncio.sleep(delay)
        else:
            # 模型落后于实时：直接让出控制权，**不补跑**（补跑会造成
            # 时间轴跳变，跨语言对账时"同一时刻"就对不上了）。
            await asyncio.sleep(0)


async def wait_listening(host, port, timeout_s=5.0):
    """轮询端口，直到服务真的在监听（或超时）。

    ★ 为什么不用"读 stdout 等同步点"：那要求主进程解析子进程输出，
    脆且容易死锁。**"端口通了"才是真的通了** —— 它同时证明了
    socket 已 bind+listen，比任何日志行都硬。
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            _r, w = await asyncio.wait_for(
                asyncio.open_connection(host, port), timeout=0.5)
            w.close()
            try:
                await w.wait_closed()
            except Exception:
                pass
            return True
        except Exception:
            await asyncio.sleep(0.05)
    return False


async def run_async(args):
    _require_pymodbus()
    store = SlaveStore(args.host, args.port, args.unit)
    model = DeviceModel(args)

    # 先用**已知值**填一遍，这样"主站连上的第一拍"就有真实数据，
    # 而不是一屏 0（0 kW 负荷是看起来完全正常的数值，最容易被当成"通了"）
    await store.write_many(model.config_writes())
    await store.write_many(model.status_writes(args))
    p_grid = model.step([0] * REG_COUNT, args.dt)
    await store.write_many(model.measurement_writes(p_grid))

    server_task = asyncio.create_task(store.serve())
    model_task = asyncio.create_task(model_loop(store, model, args))

    if not await wait_listening(args.host, args.port, timeout_s=8.0):
        print(f"SLAVE FAILED {args.host}:{args.port} 未能在 8s 内监听",
              file=sys.stderr, flush=True)
        for t in (server_task, model_task):
            t.cancel()
        await asyncio.gather(server_task, model_task, return_exceptions=True)
        return 3

    print(f"SLAVE READY {args.host}:{args.port} unit={args.unit} "
          f"load={args.load} pv={args.pv} bias={args.meter_bias} "
          f"tau={args.tau_s} ramp={args.ramp_kw_s}",
          flush=True)

    try:
        if args.duration > 0:
            await asyncio.wait_for(model_task, timeout=args.duration + 2.0)
        else:
            await asyncio.gather(server_task, model_task)
    except (asyncio.TimeoutError, asyncio.CancelledError):
        pass
    finally:
        for t in (server_task, model_task):
            t.cancel()
        await asyncio.gather(server_task, model_task, return_exceptions=True)
        try:
            await store.server.shutdown()
        except Exception:
            pass
        if args.verbose:
            print(f"  共收到 {model.cmd_received} 次指令下发，"
                  f"寄存器写 {store.writes} 次，时长 {args.duration}s",
                  flush=True)
    return 0


# =====================================================================
# 点表（机器可读形式）
#
# ★ 这里才是"点表一致性"的**真相源**：self_check() 只是它的一个人眼视图，
#   dump_tsv() 供跨语言测试做**逐字段比对**（13/tests/test_modbus_bridge.cpp T40）。
#   人眼核对不算证据 —— 功率量级的数字彼此很像，错一个寄存器用眼睛看不出来。
#
# 字段顺序（与 C++ 的 binding_tsv() 严格相同）：
#     index  name  table  address  encoding  word_order  scale  writable
# 其中 word_order 只对 f32 有意义，其余打印 "-"（避免"无意义字段"制造假红）。
# =====================================================================
def point_table():
    """按 ems_point_table.h 的索引顺序返回 32 条 (index, name, table, addr,
    encoding, word_order, scale, writable)。"""
    rows = [
        ("MEAS.P_LOAD", "IR", IR_P_LOAD, "f32", "AB", 0.0, False),
        ("MEAS.P_PV", "IR", IR_P_PV, "f32", "AB", 0.0, False),
        ("MEAS.P_BAT", "IR", IR_P_BAT, "f32", "BA", 0.0, False),
        ("MEAS.P_GRID", "IR", IR_P_GRID, "f32", "AB", 0.0, False),
        ("MEAS.SOC", "IR", IR_SOC, "u16", "-", 10000.0, False),
        ("MEAS.T_C", "IR", IR_T_C, "i16", "-", 10.0, False),
        ("MEAS.SOH", "IR", IR_SOH, "u16", "-", 10000.0, False),
        ("CMD.P_BAT", "HR", HR_P_BAT, "f32", "AB", 0.0, True),
        ("CMD.P_UPPER", "HR", HR_P_UPPER, "f32", "AB", 0.0, True),
        ("CMD.P_LOWER", "HR", HR_P_LOWER, "f32", "AB", 0.0, True),
    ]
    for i, (name, _key) in enumerate(CFG_ORDER):
        rows.append((name, "IR", IR_CFG_BASE + i * 2, "f32", "AB", 0.0, False))
    rows += [
        ("STA.BMS_COMM_OK", "DI", DI_BMS_COMM_OK, "bit", "-", 0.0, False),
        ("STA.PCS_COMM_OK", "DI", DI_PCS_COMM_OK, "bit", "-", 0.0, False),
        ("STA.METER_COMM_OK", "DI", DI_METER_COMM_OK, "bit", "-", 0.0, False),
        ("STA.PCS_FAULT", "DI", DI_PCS_FAULT, "bit", "-", 0.0, False),
        ("STA.OFFLINE", "DI", DI_OFFLINE, "bit", "-", 0.0, False),
        ("STA.DATA_VALID", "DI", DI_DATA_VALID, "bit", "-", 0.0, False),
        ("STA.BMS_CHG_FORBID", "DI", DI_BMS_CHG_FORBID, "bit", "-", 0.0, False),
        ("STA.BMS_DIS_FORBID", "DI", DI_BMS_DIS_FORBID, "bit", "-", 0.0, False),
    ]
    return [(i,) + r for i, r in enumerate(rows)]


def dump_tsv():
    """打印 TSV 点表，供跨语言测试逐字段比对（人别读这个）。"""
    out = []
    for idx, name, tbl, addr, enc, wo, scale, wr in point_table():
        out.append("\t".join([
            str(idx), name, tbl, str(addr), enc, wo,
            ("%g" % scale), ("1" if wr else "0"),
        ]))
    print("\n".join(out))
    return 0


# =====================================================================
# 自检：打印点表对照表（人眼视图；机器比对请用 --dump-tsv）
# =====================================================================
def self_check():
    print("=" * 68)
    print("13/ Python 从站点表（应与 13/src/modbus_point_map.h 逐点一致）")
    print("=" * 68)
    rows = point_table()
    for idx, name, tbl, addr, enc, wo, scale, wr in rows:
        note = {"u16": f" ×{scale:g}", "i16": f" ×{scale:g}"}.get(enc, "")
        word = " (word-swap)" if wo == "BA" else ""
        rw = " rw" if wr else ""
        star = " ★安全" if name.startswith("STA.BMS_") and name.endswith("FORBID") else ""
        print(f"  {name:<24} {tbl}[{addr:>2}]  {enc}{note}{word}{rw}{star}")
    print("=" * 68)
    print(f"  共 {len(rows)} 点（C++ 侧 EMS_POINT_COUNT = 32）")
    print("=" * 68)
    return 0 if len(rows) == 32 else 1


# =====================================================================
def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="13/ Modbus TCP 从站（PCS/BMS/电表替身）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=15020)
    p.add_argument("--unit", type=int, default=1, help="从站地址")
    p.add_argument("--dt", type=float, default=0.1, help="模型步长（秒）")
    p.add_argument("--duration", type=float, default=0.0,
                   help="运行时长，0 = 一直跑（秒）")
    p.add_argument("--verbose", action="store_true")
    p.add_argument("--self-check", action="store_true",
                   help="只打印点表对照表后退出")
    p.add_argument("--dump-tsv", action="store_true",
                   help="只打印 TSV 点表后退出（供跨语言一致性比对）")
    p.add_argument("--check-pymodbus", action="store_true",
                   help="只检查 pymodbus 是否可用后退出"
                        "（0=可用 1=缺失；供构建脚本决定 skip 而不是 fail）")

    p.add_argument("--load", type=float, default=380.0, help="负荷 (kW)")
    p.add_argument("--pv", type=float, default=150.0, help="光伏 (kW)")
    p.add_argument("--bias", type=float, default=0.0, dest="meter_bias",
                   help="关口电表系统偏差 (kW)；非 0 用于验证主站读的是电表口径")
    p.add_argument("--soc0", type=float, default=0.55)
    p.add_argument("--soh", type=float, default=0.98)
    p.add_argument("--ambient-c", type=float, default=25.0)

    p.add_argument("--cap-kwh", type=float, default=1000.0)
    p.add_argument("--pcs-max-chg", type=float, default=200.0)
    p.add_argument("--pcs-max-dis", type=float, default=200.0)
    p.add_argument("--bms-chg-lim", type=float, default=180.0)
    p.add_argument("--bms-dis-lim", type=float, default=190.0)
    p.add_argument("--tr-kva", type=float, default=250.0)
    p.add_argument("--d-target", type=float, default=250.0)
    p.add_argument("--tau-s", type=float, default=3.0)
    p.add_argument("--ramp-kw-s", type=float, default=50.0)
    p.add_argument("--standby-kw", type=float, default=2.0)
    p.add_argument("--eta-chg", type=float, default=0.96)
    p.add_argument("--eta-dis", type=float, default=0.96)
    p.add_argument("--soc-min", type=float, default=0.05)
    p.add_argument("--soc-max", type=float, default=0.95)

    p.add_argument("--pcs-fault", action="store_true", help="模拟 PCS 故障")
    p.add_argument("--offline", action="store_true", help="模拟设备离线")
    p.add_argument("--bms-chg-forbid", action="store_true", help="模拟 BMS 禁充上报")
    p.add_argument("--bms-dis-forbid", action="store_true", help="模拟 BMS 禁放上报")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.dump_tsv:
        return dump_tsv()
    if args.self_check:
        return self_check()
    if args.check_pymodbus:
        # ★ 这一条存在的理由：`--dump-tsv` **不需要** pymodbus（惰性导入），
        #   所以它能证明"python 和脚本都在"，但不能证明"服务起得来"。
        #   没有这个显式检查，"有 python 但没装 pymodbus"会通过探活，
        #   然后在起服务时失败 —— 被报成**代码缺陷**，而不是环境缺失。
        if HAVE_PYMODBUS:
            print("pymodbus OK")
            return 0
        print("pymodbus MISSING: %s" % PYMODBUS_ERROR)
        return 1
    try:
        return asyncio.run(run_async(args))
    except KeyboardInterrupt:
        print("\n(interrupted)", flush=True)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())


# =====================================================================
# 联调踩到的坑（记录在此，避免下次重踩）
#
# ① **老 datastore 路线在 3.15 里是断的**：`ModbusDeviceContext` 对传入的
#    data block 做 `deepcopy`，构造之后再改 `ir.simdata[...]` 服务端看不见，
#    而且**不报错** —— 表现是"主站永远读到 0"，是最典型的静默失败。
#    已改用非弃用路径（`pymodbus.simulator` 的 SimData/SimDevice +
#    `pymodbus.server.ModbusTcpServer`），并把 DeprecationWarning 当错误跑过一遍。
#
# ② **`SimData.count` 对 BITS 是乘数**：`count=64` 配 64 个 bool 会展开成
#    4096 位。传 1，把长度交给 `values`。
#
# ③ **更新寄存器要拿到 server 句柄**：`StartAsyncTcpServer()` 是
#    fire-and-forget，没有返回值；必须自己 `ModbusTcpServer(...)` 再
#    `serve_forever()`，才能调 `async_setValues`。
#
# ④ ★ **"注释声称的行为"必须与代码一致**：本文件初版的 `p_grid` 公式上方
#    注释写着"+ 电表系统偏差"，但公式里**根本没有加 bias**。默认 `--bias 0`
#    时"加了"和"忘了加"逐位相等，所以**默认值证明不了这一行存在**。
#    这条正是跨语言测试要验的核心：只有 bias 非 0，主站"读电表"与
#    "自己三路相减"才产生可观测差异。修完用 `--bias 40` 实测拉出 40 kW 差。
#
# ⑤ **地址基线**：老路线里 `ModbusSequentialDataBlock` 内部做 `address - 1`，
#    传 0 会抛 `TypeError: 0 <= address < 65535`。新路线用 `SimData(0, ...)`
#    直接就是协议地址 0，别再套 ±1 —— 多套一次会让所有值**整体错一个寄存器**，
#    而功率量级的数字彼此很像，肉眼发现不了。
#
# ⑥ **`simdata` 四元组写反**（我一开始判断错了，实测纠正）：
#    · 跨类型（BITS ↔ REGISTERS）→ pymodbus 构造时直接 TypeError，不静默；
#    · 同类型（CO↔DI / HR↔IR）→ 类型检查挡不住，但**也没有影响** ——
#      表身份由位置决定，而我们的读写都走同一功能码入口，
#      换掉第 1 位的对象不改变对外语义（交换 CO/DI 后联调仍 83/0 全绿）。
#    结论：**别把"元组顺序"当风险**，真正的风险是 C++ 侧 Table↔FC 对应写错。
# =====================================================================
