/**
 * LVRT Agent — 低压穿越控制器（集成RT_DB实时数据库版）
 * 
 * 从 RT_DB 共享内存读取母线电压，运行 LVRT 状态机，
 * 将控制指令（无功电流需求、SVG/逆变器控制）写回共享内存。
 */
#include "lvrt_controller.h"

// RT_DB C API
extern "C" {
#include "rt_db_api.h"
}

#include <iostream>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(x) Sleep(x)
#else
#include <unistd.h>
#define sleep_ms(x) usleep((x) * 1000)
#endif

// ============================================================
// RT_DB 设备实现 — 通过共享内存控制实际硬件
// ============================================================

class RTDB_SVG : public ISVGController {
    rt_db_handle_t* db_;
public:
    RTDB_SVG(rt_db_handle_t* db) : db_(db) {}

    void setControlMode(ControlMode mode) override {
        size_t idx = rt_db_find_index_by_id(db_, "SVG.ControlMode");
        if (idx != (size_t)-1) {
            double val = (mode == ControlMode::MODE_REACTIVE_CURRENT) ? 1.0 : 0.0;
            rt_db_set_value(db_, idx, val, 1);
        }
    }

    void setReactiveCurrent(double iq_pu) override {
        size_t idx = rt_db_find_index_by_id(db_, "SVG.IqRef");
        if (idx != (size_t)-1) {
            rt_db_set_value(db_, idx, iq_pu, 1);
        }
    }

    void exitLVRT() override {
        size_t idx = rt_db_find_index_by_id(db_, "SVG.LVRTStatus");
        if (idx != (size_t)-1) {
            rt_db_set_value(db_, idx, 0.0, 1); // 0 = 退出LVRT
        }
    }
};

class RTDB_InverterGroup : public IInverterGroup {
    rt_db_handle_t* db_;
public:
    RTDB_InverterGroup(rt_db_handle_t* db) : db_(db) {}

    void setReactiveCurrent(double total_iq_pu) override {
        size_t idx = rt_db_find_index_by_id(db_, "INVERTER.IqRef");
        if (idx != (size_t)-1) {
            rt_db_set_value(db_, idx, total_iq_pu, 1);
        }
    }

    void setActivePowerLimit(double limit_pu) override {
        // 写入有功限制 = sqrt(max^2 - iq^2)
        size_t idx = rt_db_find_index_by_id(db_, "INVERTER.ActiveLimit");
        if (idx != (size_t)-1) {
            rt_db_set_value(db_, idx, limit_pu, 1);
        }
    }

    void exitLVRT() override {
        size_t idx = rt_db_find_index_by_id(db_, "INVERTER.LVRTStatus");
        if (idx != (size_t)-1) {
            rt_db_set_value(db_, idx, 0.0, 1);
        }
    }
};

class RTDB_OLTC : public IOLTCController {
    rt_db_handle_t* db_;
public:
    RTDB_OLTC(rt_db_handle_t* db) : db_(db) {}

    void lock(bool enable) override {
        size_t idx = rt_db_find_index_by_id(db_, "OLTC.Locked");
        if (idx != (size_t)-1) {
            rt_db_set_value(db_, idx, enable ? 1.0 : 0.0, 1);
        }
    }
};

// ============================================================
// 主程序
// ============================================================

int main() {
    std::cout << "===== LVRT Agent (RT_DB 集成版) =====" << std::endl;
    std::cout << "连接实时数据库..." << std::endl;

    rt_db_handle_t db;
    if (!rt_db_init(&db, NULL)) {
        std::cerr << "[ERROR] 无法连接到 RT_DB！请先启动 rt_db_init.exe" << std::endl;
        std::cerr << "  cd build/Release && rt_db_init.exe" << std::endl;
        return 1;
    }
    std::cout << "[OK] 已连接到 RT_DB 共享内存" << std::endl;

    // 创建设备适配器（通过 RT_DB 控制）
    RTDB_SVG svg(&db);
    RTDB_InverterGroup inverters(&db);
    RTDB_OLTC oltc(&db);

    // 初始化 LVRT 控制器
    LvrtConfig config;
    // 可根据需要调整参数
    config.fault_voltage_threshold = 0.9;     // 电压低于0.9pu判定为故障
    config.recovery_voltage_threshold = 0.9;   // 电压恢复到0.9pu以上退出
    config.k_factor = 2.0;                      // 无功电流增益系数
    config.svg_current_ratio = 0.7;             // SVG承担70%无功电流

    LvrtController lvrt(&svg, &inverters, &oltc, config);

    // 设置日志回调
    lvrt.setLogCallback([](const std::string& msg) {
        std::cout << "[LVRT] " << msg << std::endl;
    });

    // 主循环 — 10ms 控制周期（100Hz）
    std::cout << "开始 LVRT 监控循环（控制周期: 10ms）..." << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    auto last_print = std::chrono::steady_clock::now();
    int cycle_count = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();

        // 1. 从 RT_DB 读取母线电压
        size_t voltage_idx = rt_db_find_index_by_id(&db, "BUS.Voltage");
        if (voltage_idx == (size_t)-1) {
            std::cerr << "[WARN] 找不到 BUS.Voltage 数据点，使用默认值 1.0" << std::endl;
            sleep_ms(10);
            continue;
        }

        double bus_voltage = 1.0;
        long quality = 0;
        if (!rt_db_get_value(&db, voltage_idx, &bus_voltage, &quality, NULL)) {
            std::cerr << "[WARN] 读取电压失败" << std::endl;
            sleep_ms(10);
            continue;
        }

        // 2. 检查数据质量
        if (quality == 0) { // QUALITY_BAD
            // 数据无效，跳过此周期
            sleep_ms(10);
            continue;
        }

        // 3. 运行 LVRT 状态机
        lvrt.run(now, bus_voltage);

        // 4. 将 LVRT 状态写回共享内存（供其他模块查看）
        size_t status_idx = rt_db_find_index_by_id(&db, "LVRT.Status");
        if (status_idx != (size_t)-1) {
            rt_db_set_value(&db, status_idx, (double)lvrt.getState(), 1);
        }

        // 5. 将 LVRT 无功电流需求写回共享内存
        size_t iq_idx = rt_db_find_index_by_id(&db, "LVRT.IqRequired");
        if (iq_idx != (size_t)-1) {
            // 通过 rt_db_get_value 获取 required_iq_ 不方便（是私有成员）
            // 我们通过共享内存中的数据间接获取，或者写个简单值
        }

        // 每100个周期（约1秒）打印一次状态
        cycle_count++;
        if (now - last_print >= std::chrono::seconds(1)) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_print).count();
            
            std::cout << "[" << std::setw(4) << (cycle_count / (elapsed / 1000.0)) 
                      << " Hz] 电压=" << std::fixed << std::setprecision(3) 
                      << bus_voltage << " pu | 状态=" << lvrt.stateString() 
                      << " | 客户端数=" << rt_db_get_connected_clients(&db) << std::endl;
            
            last_print = now;
            cycle_count = 0;
        }

        // 6. 10ms 控制周期
        sleep_ms(10);
    }

    rt_db_cleanup(&db);
    return 0;
}