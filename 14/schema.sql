-- =====================================================================
-- 14/ — 平台层数据库 schema
--
-- 依据：docs/architecture.md §22「系统数据库设计」定义的六张表
--       device / realtime_data / strategy_config / control_command /
--       alarm / energy_statistics
--
-- 本文件在 §22 之上补齐平台运行所必需的部分：
--   step_record   时序明细（10/ timeseries.csv 全字段，供历史曲线）
--   scenario      仿真场景（normal / fault）
--   economics     经济性核算（10/ summary.json 的 economics 段）
--   invariant     不变量计数（10/ summary.json 的 invariants 段）
--   app_user      用户（架构 §24 的 E 后端 / F 前端 要求「用户权限」）
--   session       登录会话（token）
--   audit_log     操作审计
--
-- 本文件在 v1.2 追加（设备接入闭环）：
--   device_conn       设备接入参数（IP/端口/从站号/轮询节奏）
--   device_point_map  Modbus 点表（32 行，13/ 的现场点表在平台侧的存档）
--
-- 本文件在 v1.4 追加（「运行模式 / 仿真模式」双模式）：
--   sim_run   仿真运行台账（谁在什么时候跑了多长的仿真、产出了哪个场景）
--
-- 约定
--   * 时刻一律用仿真时刻 t_s（秒，REAL）；time_str 是 "HH:MM" 便于展示
--     **例外（v1.2）**：scenario.time_base = 'wall' 的场景（真机实时），
--     其 t_s 是 Unix 墙钟秒、time_str 是完整日期时间。两者不可混算。
--   * 布尔一律 INTEGER 0/1
--   * 涉及点表口径的字段取自 07/src/rtdb/ems_point_table.h，不另抄一份
--     （手抄常量出过一次「24 个点全灰」的事故，见 P3/docs/README.md §7.1）
--     v1.2 的点表同样**不另抄**：14/ 以 13/docs/point_map_template.csv 为基线，
--     而该文件本身被判据 T20 断言「与 13/ 内置默认表逐字段一致」。
-- =====================================================================

PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------
-- §22 device —— 设备台账
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS device (
    device_id      TEXT PRIMARY KEY,
    device_type    TEXT NOT NULL,          -- PCS / BMS / METER / PV / TRANSFORMER / LOAD
    manufacturer   TEXT,
    model          TEXT,
    rated_power_kw REAL,                   -- PCS/电表/光伏/变压器：额定功率 kW
    capacity_kwh   REAL,                   -- BMS/电池：额定容量 kWh（其余留 NULL）
    status         TEXT DEFAULT 'offline', -- online / offline / fault / standby
    location       TEXT,
    note           TEXT
);

-- ---------------------------------------------------------------------
-- device_conn —— 设备接入参数（对下 Modbus TCP，v1.2）
--
-- 为什么与 device 台账分开：
--   台账是「这台设备是什么」（型号/容量/位置），接入参数是「怎么连上它」
--   （IP/端口/从站号）。现场常常先登记台账、后调试接入，混在一张表里会让
--   「设备已录入」和「设备已接通」两件事分不开。
--
-- 粒度：**一条连接一份点表**。
--   13/ 的 ModbusDeviceIO::Config 只有一组 host/port/unit_id
--   （注释原话「从站地址（现场一台设备一个）」），所以一次装配 = 一组接
--   入参数 + 一份点表。现实中 PCS / BMS / 关口电表 是三台设备、三条连接，
--   那时就是三行 device_conn、三份 device_point_map。本表按 device_id 建主键，
--   天然支持多条；v1.2 先落一条。
--
-- point_map_path 为空 = 该设备尚未配置点表，13/ 会用编译期内置默认表。
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS device_conn (
    device_id       TEXT PRIMARY KEY,
    protocol        TEXT NOT NULL DEFAULT 'modbus_tcp',
    host            TEXT,                  -- 设备 IP（厂家协议文档里给）
    port            INTEGER DEFAULT 502,
    unit_id         INTEGER DEFAULT 1,     -- 从站号
    poll_period_ms  INTEGER DEFAULT 100,   -- 轮询周期（与控制周期对齐）
    timeout_ms      INTEGER DEFAULT 1000,  -- 单次请求超时
    auto_reconnect  INTEGER DEFAULT 1,
    enabled         INTEGER DEFAULT 1,
    point_map_path  TEXT,                  -- 点表落盘路径（13/ 启动时读它）
    point_map_rows  INTEGER DEFAULT 0,     -- 点表行数（0 = 未配置）
    point_map_saved_at TEXT,               -- 点表最后保存时刻
    updated_at      TEXT,
    note            TEXT,
    FOREIGN KEY (device_id) REFERENCES device(device_id)
);

-- ---------------------------------------------------------------------
-- device_point_map —— Modbus 点表存档（v1.2）
--
-- 这是 13/ 现场点表在平台侧的**权威副本**。客户在 15/ 界面上改的就是它，
-- 保存时由 14/ 连同落盘的 CSV 一起更新，13/ 启动时读那份 CSV。
--
-- 行序即索引：idx 0..31 与 13/ 的 EMS_* 枚举、与 EMS_POINT_NAMES 的
--   前 32 项（设备侧区段，EMS_EXT_BEGIN 之前）一一对应。**不允许删行/换行**。
-- name 列是两侧对齐的锚点，改它等于换了点表，必须连同 C++ 侧一起改。
--
-- 取值域与 13/src/modbus_point_map.h 的 table_code()/encoding_code()/
--   word_order_code() 完全一致，不另发明写法：
--     table      IR / HR / DI / CO
--     encoding   f32 / u16 / i16 / bit
--     word_order low / high        （仅 f32 有意义，现场第一大坑）
--     scale      f32 与 bit 必须为 0；u16/i16 必须 > 0（物理值 = raw / scale）
--     writable   rw / ro
--   scale 存 REAL 而不是 TEXT，是为了能直接参与比较与展示；
--   写入时的合法性由 14/src/pointmap.py 与 13/ 的 validate_map() 双重把关。
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS device_point_map (
    device_id   TEXT NOT NULL,
    idx         INTEGER NOT NULL,          -- 0..31，行序即索引
    name        TEXT NOT NULL,             -- 点名（两侧对齐的锚点）
    table_kind  TEXT NOT NULL,             -- IR / HR / DI / CO
    address     INTEGER NOT NULL,          -- 0 起算（厂家手册 30001/40001 要减基数）
    encoding    TEXT NOT NULL,             -- f32 / u16 / i16 / bit
    word_order  TEXT NOT NULL DEFAULT 'high',
    scale       REAL NOT NULL DEFAULT 0,
    writable    INTEGER NOT NULL DEFAULT 0,
    unit        TEXT,
    note        TEXT,
    PRIMARY KEY (device_id, idx),
    FOREIGN KEY (device_id) REFERENCES device(device_id)
);

CREATE INDEX IF NOT EXISTS idx_pmap_dev ON device_point_map(device_id, idx);

-- ---------------------------------------------------------------------
-- §22 realtime_data —— 实时数据
--   一次写入包含「值 + 品质 + 时刻」三要素，与 RT_DB 的写入模型一致
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS realtime_data (
    ts            REAL NOT NULL,           -- 仿真时刻 t_s
    device_id     TEXT NOT NULL,
    power_kw      REAL,
    voltage_v     REAL,
    current_a     REAL,
    soc           REAL,
    temperature_c REAL,
    status        TEXT,
    quality_ok    INTEGER DEFAULT 1,       -- 0 = 本拍数据不可信（丢帧/越界）
    PRIMARY KEY (ts, device_id),
    FOREIGN KEY (device_id) REFERENCES device(device_id)
);

CREATE INDEX IF NOT EXISTS idx_rt_ts  ON realtime_data(ts);
CREATE INDEX IF NOT EXISTS idx_rt_dev ON realtime_data(device_id, ts);

-- ---------------------------------------------------------------------
-- run_mode —— 运行模式（独立顶层业务概念，非「数据场景」）
--   区分「电池在干哪种生意」，与 scenario（哪份数据）正交：
--     scenario  = normal / fault   —— 描述今天工况好不好（数据维度）
--     run_mode  = 套利 / 削峰填谷 / 辅助服务 / 保电备电 —— 描述商业模式（业务维度）
--   每种模式有：目标函数（最大化什么）、核心约束、结算口径（钱怎么算）、
--   绑定的策略组（切换模式即联动启停 strategy_config）、适用的客户类型。
--   一套 EMS 支持四种模式，是同一套软件、不是四个割裂的产品。
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS run_mode (
    mode_id          TEXT PRIMARY KEY,        -- arbitrage / peak_shaving / ancillary / backup
    name             TEXT NOT NULL,           -- 峰谷套利 / 削峰填谷 / 辅助服务 / 保电备电
    customer_type    TEXT NOT NULL,           -- 卖电型 / 自用型 / 卖电型 / 保供型
    objective        TEXT NOT NULL,           -- 目标函数（一句话）
    constraints      TEXT,                    -- 核心约束（JSON 数组）
    settlement       TEXT NOT NULL,           -- 结算口径：钱怎么算
    kpi_key          TEXT NOT NULL,           -- 主 KPI 字段名（前端按此切口径）
    bound_strategies TEXT NOT NULL,           -- 绑定的策略 ID（JSON 数组，切换时联动启停）
    applicable_scene TEXT NOT NULL DEFAULT 'normal',  -- 适用场景（可多个，逗号分隔）
    is_default       INTEGER DEFAULT 0,       -- 默认模式
    active           INTEGER DEFAULT 0,       -- 当前激活（全局唯一激活）
    description      TEXT
);

-- ---------------------------------------------------------------------
-- §22 strategy_config —— 策略配置（对应 04/ 的 9 个策略）
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS strategy_config (
    strategy_id   TEXT PRIMARY KEY,
    strategy_type TEXT NOT NULL,           -- 策略类名
    priority      TEXT NOT NULL,           -- L0 / L1 / L2 / L3
    enabled       INTEGER DEFAULT 1,
    parameters    TEXT,                    -- JSON 文本
    description   TEXT,
    updated_at    TEXT,
    mode_id       TEXT DEFAULT NULL        -- 归属的运行模式（安全底座策略为 NULL，全模式共用）
);

-- ---------------------------------------------------------------------
-- §22 control_command —— 控制指令（下发 vs 实际）
--   result 口径与 07/ StepRecord 的四个布尔对齐：
--     ok / clamped / safety_clip / state_gated / hold_last
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS control_command (
    scenario_id     TEXT NOT NULL DEFAULT 'normal',
    ts              REAL NOT NULL,
    device_id       TEXT NOT NULL,
    target_power_kw REAL,                  -- 指令值 p_cmd
    actual_power_kw REAL,                  -- 实际值 p_actual
    p_lower_kw      REAL,                  -- 本拍权限下界
    p_upper_kw      REAL,                  -- 本拍权限上界
    result          TEXT,                  -- ok / clamped / gated / hold_last
    reason          TEXT,
    PRIMARY KEY (scenario_id, ts, device_id),
    FOREIGN KEY (device_id) REFERENCES device(device_id)
);

CREATE INDEX IF NOT EXISTS idx_cmd_ts ON control_command(ts);

-- ---------------------------------------------------------------------
-- §22 alarm —— 告警（来源：10/ alarms.csv + P2 的 SOE）
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS alarm (
    alarm_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          REAL NOT NULL,
    time_str    TEXT,
    level       TEXT NOT NULL,             -- INFO / WARNING / DERATED / FAULT / EMERGENCY
    source      TEXT,
    device_id   TEXT,
    alarm_type  TEXT,
    description TEXT,
    status      TEXT DEFAULT 'active',     -- active / cleared
    scenario_id TEXT DEFAULT 'normal'
);

CREATE INDEX IF NOT EXISTS idx_alarm_ts    ON alarm(ts);
CREATE INDEX IF NOT EXISTS idx_alarm_level ON alarm(level);

-- ---------------------------------------------------------------------
-- §22 energy_statistics —— 能量统计（按日）
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS energy_statistics (
    date            TEXT NOT NULL,         -- YYYY-MM-DD
    scenario_id     TEXT NOT NULL DEFAULT 'normal',
    charge_kwh      REAL DEFAULT 0,
    discharge_kwh   REAL DEFAULT 0,
    pv_kwh          REAL DEFAULT 0,
    load_kwh        REAL DEFAULT 0,
    grid_kwh        REAL DEFAULT 0,
    export_kwh      REAL DEFAULT 0,
    peak_grid_kw    REAL DEFAULT 0,
    net_benefit_cny REAL DEFAULT 0,
    PRIMARY KEY (date, scenario_id)
);

-- ---------------------------------------------------------------------
-- step_record —— 时序明细（曲线页的数据源）
--   字段与 10/ timeseries.csv 逐列对应，顺序也一致
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS step_record (
    scenario_id  TEXT NOT NULL DEFAULT 'normal',
    t_s          REAL NOT NULL,
    time_str     TEXT,
    state        TEXT,
    p_load_kw    REAL,
    p_pv_kw      REAL,
    p_grid_kw    REAL,
    p_cmd_kw     REAL,
    p_actual_kw  REAL,
    soc          REAL,
    temp_c       REAL,
    p_lower_kw   REAL,
    p_upper_kw   REAL,
    plan_target  REAL,
    correction   REAL,
    clamped      INTEGER,
    safety_clip  INTEGER,
    state_gated  INTEGER,
    hold_last    INTEGER,
    fault_bits   INTEGER,
    reason       TEXT,
    PRIMARY KEY (scenario_id, t_s)
);

CREATE INDEX IF NOT EXISTS idx_step_state ON step_record(scenario_id, state);

-- ---------------------------------------------------------------------
-- scenario —— 数据场景
--   time_base 区分时刻口径（v1.2）：
--     'sim'  = 10/ 离线仿真，t_s 是一天内的秒（0..86400），10 秒一拍
--     'wall' = 07/ 现场进程实录（--record），t_s 是 Unix 墙钟秒
--   为什么必须有这个字段：两套时间轴不可混算。仿真场景按"一天的第几秒"画
--   曲线是对的，真机数据按同一套逻辑画就是错的（它跨月、跨年，还没有
--   "第 0 秒"）。加字段而不是换算法，是为了让**仿真场景原样保留**。
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS scenario (
    scenario_id      TEXT PRIMARY KEY,
    name             TEXT NOT NULL,
    source_dir       TEXT,
    time_base        TEXT NOT NULL DEFAULT 'sim',   -- sim / wall
    steps            INTEGER,
    log_rows         INTEGER,
    wall_s           REAL,
    ok               INTEGER,
    alarms_total     INTEGER,
    alarms_fault     INTEGER,
    fault_ticks      INTEGER,
    invariants_all_ok INTEGER,
    imported_at      TEXT
);

-- ---------------------------------------------------------------------
-- sim_run —— 仿真运行台账（v1.4）
--
--   为什么单独一张表，而不是复用 scenario：
--     scenario 描述的是**数据**（有一份时序、一份经济性），
--     sim_run 描述的是**一次运行**（谁、什么时候、按什么时长与粒度跑的）。
--     一次运行产出一个数据集。混在一张表里，以后想加"运行参数快照"
--     就只能往 scenario 上继续堆列，而 scenario 也被 07/ 现场实录共用着。
--
--   status：running（跑着） / ok（正常结束） / failed（失败，error 有原因）
--   log_every：日志降采样（单位=控制拍）。日/周=10（10 s 粒度），
--              月=60（60 s）—— 粒度存在台账里，界面才能显示**真实**粒度，
--              而不是让人以为月报也是 10 s 的。
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS sim_run (
    run_id        TEXT PRIMARY KEY,          -- d001 / w002 / m003
    kind          TEXT NOT NULL,             -- day / week / month
    scenario_id   TEXT NOT NULL,             -- sim-d001
    fault         INTEGER NOT NULL DEFAULT 0,
    duration_s    REAL NOT NULL,
    dt_s          REAL NOT NULL,
    log_every     INTEGER NOT NULL,
    status        TEXT NOT NULL,
    started_at    TEXT NOT NULL,
    finished_at   TEXT,
    wall_s        REAL,
    steps         INTEGER,
    log_rows      INTEGER,
    alarms_total  INTEGER,
    net_benefit_cny   REAL,
    saving_total_cny  REAL,
    invariants_all_ok INTEGER,
    artifact_dir  TEXT,
    username      TEXT,
    error         TEXT
);

CREATE INDEX IF NOT EXISTS idx_simrun_started ON sim_run(started_at);

-- ---------------------------------------------------------------------
-- economics —— 经济性核算（10/ summary.json 的 economics 段）
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS economics (
    scenario_id          TEXT PRIMARY KEY,
    e_import_kwh         REAL, e_export_kwh      REAL,
    e_charge_kwh         REAL, e_discharge_kwh   REAL,
    throughput_kwh       REAL, equiv_cycles      REAL,
    pv_self_use_kwh      REAL,
    peak_grid_kw         REAL, peak_grid_base_kw REAL,
    cost_energy_cny      REAL, cost_demand_cny   REAL,
    revenue_feed_in_cny  REAL,
    cost_total_cny       REAL,
    cost_energy_base_cny REAL, cost_demand_base_cny REAL,
    revenue_feed_in_base_cny REAL,
    cost_total_base_cny  REAL,
    saving_energy_cny    REAL, saving_demand_cny REAL,
    saving_total_cny     REAL,
    cost_degradation_cny REAL, net_benefit_cny   REAL,
    saving_pct           REAL
);

-- ---------------------------------------------------------------------
-- invariant —— 不变量计数（安全红线，全 0 才算过）
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS invariant (
    scenario_id         TEXT PRIMARY KEY,
    out_of_interval     INTEGER,   -- 指令越出权限区间
    over_limit          INTEGER,   -- 越设备限值
    gated_nonzero       INTEGER,   -- 门控期仍有非零输出
    grid_breach         INTEGER,   -- 关口越契约需量
    grid_breach_soc_limited INTEGER,
    tr_breach           INTEGER,   -- 变压器越限
    soc_violation       INTEGER,   -- SOC 越界
    hard_ok             INTEGER,
    safety_ok           INTEGER,
    all_ok              INTEGER
);

-- ---------------------------------------------------------------------
-- app_user / session / audit_log —— 用户与权限
--   role: admin（全部）/ operator（可写配置与指令）/ viewer（只读）
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS app_user (
    user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,        -- pbkdf2_hmac(sha256)
    salt          TEXT NOT NULL,
    iterations    INTEGER NOT NULL,
    role          TEXT NOT NULL DEFAULT 'viewer',
    display_name  TEXT,
    enabled       INTEGER DEFAULT 1,
    created_at    TEXT
);

CREATE TABLE IF NOT EXISTS session (
    token      TEXT PRIMARY KEY,
    user_id    INTEGER NOT NULL,
    created_at TEXT,
    expires_at TEXT,
    FOREIGN KEY (user_id) REFERENCES app_user(user_id)
);

CREATE TABLE IF NOT EXISTS audit_log (
    id       INTEGER PRIMARY KEY AUTOINCREMENT,
    ts       TEXT,
    username TEXT,
    action   TEXT,
    target   TEXT,
    detail   TEXT
);

CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_log(ts);

-- ---------------------------------------------------------------------
-- 元信息：记录导入来源与版本，便于「换人不靠口头交接」
-- ---------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
