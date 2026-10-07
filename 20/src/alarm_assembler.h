// =====================================================================
// 20/ — 告警装配器 AlarmAssembler（与仿真无关）
//
// ---------------------------------------------------------------------
// 为什么入参不是 Sim24hConfig
// ---------------------------------------------------------------------
// 10/ 的 `collect_alarms(rt, const Sim24hConfig&, log, out)` 把两条完全不同的
// 职责焊在了一起：
//     ① "把物理量/状态位翻译成告警"（**业务逻辑**）
//     ② "从仿真日志取物理量"           （**装配逻辑**）
// 本文件只做 ①，输入是 **AlarmInput**（一张拍快照 + 上一拍快照），
// 与仿真结构体无关；② 由 alarm_input.h 的两个适配器分别承担
// （一个从 10/ 的 StepRecord 来，一个从 P0 的 RealtimeSnapshot/DeviceStatus 来）。
//
// ---------------------------------------------------------------------
// 边沿语义（本文件的核心不变量）
// ---------------------------------------------------------------------
//   · **SET/CLEAR 成对**：条件上升沿出 1 条 SET，下降沿出 1 条 CLEAR；
//   · **持续期间不重复刷屏**：条件保持成立 N 拍只产生 1 条 SET，
//     `AlarmRecord::repeat` 记 N，最后一条 CLEAR 收尾。
//     （P2 实测收紧配置下 safety_clip 连续 838 拍 —— 逐拍记就是 838 条。）
//   · 阈值类带**回差**（hysteresis），避免在边界抖动时反复 SET/CLEAR。
//   · 状态迁移是**点事件**（每次迁移独立成条，不参与配对）——
//     合并迁移会丢掉 A→B→C 这条链。
//
// 编译：纯头文件，实现全部 inline。
// =====================================================================

#pragma once

#include "alarm_model.h"
#include "data_models.h"    // Timestamp
#include "state_machine.h"  // EmsState

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace ems {

// =====================================================================
// 告警判定阈值（与仿真无关；生产侧由 SafetyParams / DeviceLimits 填充）
// =====================================================================
struct AlarmContext {
    double soc_low            = 0.15;
    double soc_high           = 0.85;
    double soc_hysteresis     = 0.02;

    double temp_warn_c        = 45.0;
    double temp_fault_c       = 55.0;
    double temp_hysteresis_c  = 2.0;

    // 变压器：负载 = |p_grid| + tr_load_pv_share * p_load（沿用 04/05/10 的简化口径）
    double transformer_capacity_kw = 0.0;   // <=0 表示不判变压器
    double tr_overload_th          = 0.95;
    double tr_load_pv_share        = 0.10;
    double tr_hysteresis           = 0.02;

    // 需量：p_grid > demand_target_kw 即越契约
    double demand_target_kw = 0.0;          // <=0 表示不判需量

    // 防逆流：不允许倒送时，p_grid < grid_min_kw - eps 报警
    bool   forbid_reverse        = false;
    double grid_min_kw           = 0.0;
    double grid_reverse_eps_kw   = 1.0;

    // 是否让"实测与上拍不同"之类的小抖动参与判定；默认严格（只判阈值）
    bool   enable_state_transition = true;
};

// =====================================================================
// 与仿真无关的输入快照
//
//   把"告警判定真正需要的东西"列全 —— 只有物理量、状态位、状态迁移。
//   换入口（11/ 的 main_ems.cpp、14/ 的平台后端）时，只要能把它们的
//   快照映射成这个结构，告警能力就跟着走。
// =====================================================================
struct AlarmInput {
    double t = 0.0;

    // ---- 状态（来自 06/ 状态机）----
    EmsState    state      = EmsState::kInit;
    bool        has_transition = false;   // 本拍是否发生迁移
    EmsState    state_from = EmsState::kInit;
    EmsState    state_to   = EmsState::kInit;
    std::string transition_reason;

    // ---- 物理量 ----
    double soc           = 0.5;
    double temperature_c = 25.0;
    double p_grid_kw     = 0.0;
    double p_load_kw     = 0.0;

    // ---- 设备 / 通信状态（P0 DeviceStatus 的语义）----
    bool bms_comm_ok    = true;
    bool pcs_comm_ok    = true;
    bool meter_comm_ok  = true;
    bool pcs_fault      = false;
    bool device_offline = false;
    bool data_valid     = true;   // 数据品质位
    bool emergency_stop = false;  // 外部急停 / 温度故障等紧急源

    // ---- 运行期派生标志 ----
    bool data_stale  = false;   // 采集超时 → HOLD_LAST 生效
    bool safety_clip = false;   // 本拍安全层限幅真正动作
};

// =====================================================================
// AlarmAssembler
// =====================================================================
class AlarmAssembler {
public:
    struct Config {
        // 边沿检测。★ 关掉它 = "每拍都记一条"，用于**判据有效性自证**：
        //   测试里两个模式各跑 N 拍，assert 开=1 条 / 关=N 条。
        //   如果有人把边沿检测改坏（退化成逐拍记录），"开"的那条断言立刻红。
        bool edge_detection = true;
    };

    AlarmAssembler() = default;
    explicit AlarmAssembler(const AlarmContext& ctx) : ctx_(ctx) {}
    AlarmAssembler(const AlarmContext& ctx, const Config& cfg) : ctx_(ctx), cfg_(cfg) {}

    void set_context(const AlarmContext& ctx) { ctx_ = ctx; }
    const AlarmContext& context() const { return ctx_; }
    void set_config(const Config& cfg) { cfg_ = cfg; }
    const Config& config() const { return cfg_; }

    void reset() {
        records_.clear();
        active_.clear();
        next_id_ = 1;
    }

    // -----------------------------------------------------------------
    // 推进一拍。
    //   cur  —— 当前拍快照
    //   prev —— 上一拍快照（可为 nullptr；仅用于在 cur.has_transition==false
    //           时兜底推断迁移。装配器**同时**持有上一拍的报警状态，
    //           两者缺一不可：prev 给状态迁移，active_ 给边沿）。
    // 返回本拍产生的离散事件（SET / CLEAR），顺序稳定。
    // -----------------------------------------------------------------
    std::vector<AlarmEvent> update(const AlarmInput& cur, const AlarmInput* prev = nullptr) {
        std::vector<AlarmEvent> events;

        // ---------- ① 状态迁移（点事件）----------
        if (ctx_.enable_state_transition) {
            EmsState from = cur.state_from, to = cur.state_to;
            bool has = cur.has_transition;
            if (!has && prev && prev->state != cur.state) {
                has = true;
                from = prev->state;
                to   = cur.state;
            }
            if (has && to != from) {
                AlarmRecord r;
                r.id        = next_id_++;
                r.code      = AlarmCode::kFsmTransition;
                r.clear_code= AlarmCode::kNone;
                r.severity  = alarm_severity_for_state(to);
                r.action    = (to == EmsState::kFault || to == EmsState::kEmergency)
                                  ? AlarmAction::kGateZero : AlarmAction::kNone;
                r.owner     = AlarmOwner::kEms;
                r.source    = AlarmSource::kFsm;
                r.source_id = "FSM";
                r.t_set     = cur.t;
                r.t_clear   = cur.t;          // 点事件：SET==CLEAR
                r.active    = false;
                r.repeat    = 1;
                r.message   = std::string("state ") + state_name(from) + " -> " +
                              state_name(to) + (cur.transition_reason.empty()
                                  ? "" : (" (" + cur.transition_reason + ")"));
                records_.push_back(r);

                AlarmEvent ev;
                ev.t = cur.t; ev.code = r.code; ev.severity = r.severity;
                ev.source = r.source; ev.source_id = r.source_id;
                ev.is_set = true; ev.pair_id = r.id;
                ev.message = r.message;
                events.push_back(ev);
            }
        }

        // ---------- ② 条件类（有 SET / CLEAR 生命周期）----------
        // 先收集本拍**成立**的条件
        std::vector<Cond> conds;
        eval_conditions(cur, conds);

        std::vector<std::string> seen;
        seen.reserve(conds.size());

        for (const auto& c : conds) {
            const std::string k = c.key();
            seen.push_back(k);

            auto it = cfg_.edge_detection ? active_.find(k) : active_.end();
            if (it != active_.end()) {
                // 持续存在：只更新重复计数与最新值，**不产生事件**
                AlarmRecord& r = records_[it->second];
                ++r.repeat;
                r.value   = c.value;
                r.message = c.message;
                continue;
            }
            // 上升沿（或关闭边沿检测时的每一拍）→ 新 SET
            AlarmRecord r;
            r.id         = next_id_++;
            r.code       = c.code;
            r.clear_code = alarm_clear_code(c.code);
            r.severity   = c.severity;
            r.action     = c.action;
            r.owner      = c.owner;
            r.source     = c.source;
            r.source_id  = c.source_id;
            r.t_set      = cur.t;
            r.active     = true;
            r.value      = c.value;
            r.repeat     = 1;
            r.message    = c.message;
            const size_t idx = records_.size();
            records_.push_back(r);
            if (cfg_.edge_detection) active_[k] = idx;

            AlarmEvent ev;
            ev.t = cur.t; ev.code = r.code; ev.severity = r.severity;
            ev.source = r.source; ev.source_id = r.source_id;
            ev.is_set = true; ev.pair_id = r.id;
            ev.value = r.value; ev.message = r.message;
            events.push_back(ev);
        }

        // ---------- ③ 下降沿 → CLEAR ----------
        if (cfg_.edge_detection) {
            std::vector<std::string> gone;
            for (auto& kv : active_) {
                if (std::find(seen.begin(), seen.end(), kv.first) == seen.end())
                    gone.push_back(kv.first);
            }
            std::sort(gone.begin(), gone.end());   // 顺序稳定（unordered_map 无序）
            for (const auto& k : gone) {
                const size_t idx = active_[k];
                AlarmRecord& r = records_[idx];
                r.active  = false;
                r.t_clear = cur.t;
                const AlarmCode cc = (r.clear_code == AlarmCode::kNone)
                                         ? r.code : r.clear_code;
                AlarmEvent ev;
                ev.t = cur.t; ev.code = cc; ev.severity = r.severity;
                ev.source = r.source; ev.source_id = r.source_id;
                ev.is_set = false; ev.pair_id = r.id;
                ev.value = r.value;
                ev.message = std::string("cleared: ") + alarm_code_name(r.code);
                events.push_back(ev);
                active_.erase(k);
            }
        }
        return events;
    }

    // ---- 只读视图 ----
    const std::vector<AlarmRecord>& records() const { return records_; }

    std::vector<const AlarmRecord*> active() const {
        std::vector<const AlarmRecord*> v;
        for (const auto& r : records_) if (r.active) v.push_back(&r);
        return v;
    }

    size_t size() const { return records_.size(); }

    // 按 (source, code) 计数（不是按拍）
    int count_code(AlarmCode c) const {
        int n = 0;
        for (const auto& r : records_) if (r.code == c) ++n;
        return n;
    }
    int count_at_least(AlarmSeverity s) const {
        int n = 0;
        for (const auto& r : records_) if (r.severity >= s) ++n;
        return n;
    }
    // 是否出现过某个 CLEAR 事件（来自 code 所对的 Set）
    bool cleared_code(AlarmCode set_code) const {
        for (const auto& r : records_)
            if (r.code == set_code && r.cleared()) return true;
        return false;
    }

private:
    // 条件键：必须与 `AlarmAssembler::active_` 的键**逐字一致**，否则回差
    // 查询挂在空键上 → 滞环静默失效（本项目踩过一次，见 README 坑 6）。
    static std::string cond_key(AlarmSource src, const std::string& sid, AlarmCode code) {
        return std::string(alarm_source_name(src)) + ":" + sid + ":" + alarm_code_name(code);
    }

    struct Cond {
        AlarmCode     code;
        AlarmSeverity severity;
        AlarmAction   action;
        AlarmOwner    owner;
        AlarmSource   source;
        std::string   source_id;
        std::string   message;
        double        value;
        std::string key() const { return cond_key(source, source_id, code); }
    };

    // 带**回差**的条件存在性：越限立即成立；只有当量回到
    // (阈值 ∓ 回差) 之外才解除。没有 active_ 记录时（关边沿检测）退化为纯阈值。
    bool latched(const std::string& k, bool bad, bool definitely_ok) const {
        if (bad) return true;
        if (!cfg_.edge_detection) return false;
        if (active_.find(k) == active_.end()) return false;
        return !definitely_ok;
    }

    void eval_conditions(const AlarmInput& cur, std::vector<Cond>& out) const {
        auto add = [&out](AlarmCode code, AlarmSource src, const std::string& sid,
                          const std::string& msg, double val) {
            const AlarmPolicy p = alarm_policy(code);
            Cond c;
            c.code = code; c.severity = p.severity; c.action = p.action; c.owner = p.owner;
            c.source = src; c.source_id = sid; c.message = msg; c.value = val;
            out.push_back(c);
        };

        char buf[160];

        // ---- 通信 / 故障类（六类故障源的机器可读形式，见 alarm_model.h §4）----
        if (!cur.bms_comm_ok)
            add(AlarmCode::kCommLostBms, AlarmSource::kComm, "BMS", "bms comm lost", 0.0);
        if (!cur.pcs_comm_ok)
            add(AlarmCode::kCommLostPcs, AlarmSource::kComm, "PCS", "pcs comm lost", 0.0);
        if (!cur.meter_comm_ok)
            add(AlarmCode::kCommLostMeter, AlarmSource::kComm, "METER", "meter comm lost", 0.0);
        if (cur.pcs_fault)
            add(AlarmCode::kPcsFaultSet, AlarmSource::kDevice, "PCS", "pcs fault", 0.0);
        if (cur.device_offline)
            add(AlarmCode::kDeviceOffline, AlarmSource::kDevice, "PLANT", "device offline", 0.0);
        if (!cur.data_valid) {
            std::snprintf(buf, sizeof(buf), "data invalid (quality=bad)");
            add(AlarmCode::kDataStaleStart, AlarmSource::kComm, "ACQ", buf, 0.0);
        }
        if (cur.emergency_stop)
            add(AlarmCode::kFsmEmergencyStop, AlarmSource::kSystem, "ESTOP",
                "emergency stop", 0.0);

        // ---- 采集超时 → HOLD_LAST（降级路径本身）----
        if (cur.data_stale)
            add(AlarmCode::kHoldLastStart, AlarmSource::kComm, "METER", "hold_last engaged", 0.0);

        // ---- SOC（阈值 + 回差）----
        {
            const bool hi_bad = cur.soc >= ctx_.soc_high;
            const bool hi_ok  = cur.soc <  ctx_.soc_high - ctx_.soc_hysteresis;
            std::snprintf(buf, sizeof(buf), "soc high soc=%.4f >= %.4f", cur.soc, ctx_.soc_high);
            if (latched(cond_key(AlarmSource::kSafety, "SOC", AlarmCode::kSocHighStart),
                        hi_bad, hi_ok))
                add(AlarmCode::kSocHighStart, AlarmSource::kSafety, "SOC", buf, cur.soc);

            const bool lo_bad = cur.soc <= ctx_.soc_low;
            const bool lo_ok  = cur.soc >  ctx_.soc_low + ctx_.soc_hysteresis;
            std::snprintf(buf, sizeof(buf), "soc low soc=%.4f <= %.4f", cur.soc, ctx_.soc_low);
            if (latched(cond_key(AlarmSource::kSafety, "SOC", AlarmCode::kSocLowStart),
                        lo_bad, lo_ok))
                add(AlarmCode::kSocLowStart, AlarmSource::kSafety, "SOC", buf, cur.soc);
        }

        // ---- 温度：预警 + 故障（故障走 EMERGENCY，与 06/faults.emergency() 一致）----
        {
            const bool warn_bad = cur.temperature_c >= ctx_.temp_warn_c;
            const bool warn_ok  = cur.temperature_c < ctx_.temp_warn_c - ctx_.temp_hysteresis_c;
            std::snprintf(buf, sizeof(buf), "temp high t=%.2f >= %.2f",
                          cur.temperature_c, ctx_.temp_warn_c);
            if (latched(cond_key(AlarmSource::kDevice, "BMS", AlarmCode::kTempHighStart),
                        warn_bad, warn_ok))
                add(AlarmCode::kTempHighStart, AlarmSource::kDevice, "BMS", buf,
                    cur.temperature_c);

            if (cur.temperature_c >= ctx_.temp_fault_c) {
                std::snprintf(buf, sizeof(buf), "temp fault t=%.2f >= %.2f",
                              cur.temperature_c, ctx_.temp_fault_c);
                add(AlarmCode::kFsmEmergencyStop, AlarmSource::kDevice, "BMS_TEMP", buf,
                    cur.temperature_c);
            }
        }

        // ---- 变压器过载（负载 = |p_grid| + share*p_load，04/05/10 同口径）----
        if (ctx_.transformer_capacity_kw > 0.0) {
            const double tr_load = std::fabs(cur.p_grid_kw) +
                                   ctx_.tr_load_pv_share * cur.p_load_kw;
            const double lim = ctx_.transformer_capacity_kw * ctx_.tr_overload_th;
            const bool bad = tr_load > lim;
            const bool ok  = tr_load <= lim * (1.0 - ctx_.tr_hysteresis);
            std::snprintf(buf, sizeof(buf), "transformer overload load=%.2f > %.2f",
                          tr_load, lim);
            if (latched(cond_key(AlarmSource::kSystem, "TRANSFORMER",
                                 AlarmCode::kTrOverloadStart), bad, ok))
                add(AlarmCode::kTrOverloadStart, AlarmSource::kSystem, "TRANSFORMER", buf, tr_load);
        }

        // ---- 需量越契约 ----
        if (ctx_.demand_target_kw > 0.0) {
            const bool bad = cur.p_grid_kw > ctx_.demand_target_kw;
            const bool ok  = cur.p_grid_kw <= ctx_.demand_target_kw * 0.98;
            std::snprintf(buf, sizeof(buf), "demand breach grid=%.2f > %.2f",
                          cur.p_grid_kw, ctx_.demand_target_kw);
            if (latched(cond_key(AlarmSource::kEcon, "DEMAND", AlarmCode::kDemandBreachStart),
                        bad, ok))
                add(AlarmCode::kDemandBreachStart, AlarmSource::kEcon, "DEMAND", buf,
                    cur.p_grid_kw);
        }

        // ---- 防逆流（关口越下限）----
        if (ctx_.forbid_reverse &&
            cur.p_grid_kw < ctx_.grid_min_kw - ctx_.grid_reverse_eps_kw) {
            std::snprintf(buf, sizeof(buf), "grid reverse p_grid=%.2f < %.2f",
                          cur.p_grid_kw, ctx_.grid_min_kw);
            add(AlarmCode::kGridReverseStart, AlarmSource::kSafety, "GRID", buf, cur.p_grid_kw);
        }

        // ---- 安全层限幅生效（说明策略与安全边界冲突，值得关注）----
        if (cur.safety_clip)
            add(AlarmCode::kSafetyClipStart, AlarmSource::kSafety, "SAFETY",
                "safety clip active", 0.0);
    }

    AlarmContext ctx_{};
    Config       cfg_{};
    std::vector<AlarmRecord> records_;
    std::unordered_map<std::string, size_t> active_;
    long long    next_id_ = 1;
};

} // namespace ems
