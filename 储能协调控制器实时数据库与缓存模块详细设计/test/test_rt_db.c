#include "rt_db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define sleep(x) Sleep((x) * 1000)
#else
#include <unistd.h>
#endif

// 测试结果统计
typedef struct {
    int total_tests;
    int passed_tests;
    int failed_tests;
} TestResults;

static TestResults g_test_results = {0, 0, 0};

// 测试辅助宏
#define TEST_START(name) \
    do { \
        printf("\\n[TEST] %s: ", name); \
        g_test_results.total_tests++; \
    } while(0)

#define TEST_ASSERT(condition, message) \
    do { \
        if (condition) { \
            printf("PASS - %s\\n", message); \
            g_test_results.passed_tests++; \
        } else { \
            printf("FAIL - %s\\n", message); \
            g_test_results.failed_tests++; \
            return false; \
        } \
    } while(0)

#define TEST_END() \
    do { \
        return true; \
    } while(0)

// 打印测试统计
void print_test_summary(void) {
    printf("\n");
    for(int i = 0; i < 50; i++) printf("=");
    printf("\n");
    printf("TEST SUMMARY\n");
    for(int i = 0; i < 50; i++) printf("=");
    printf("\n");
    printf("Total Tests: %d\n", g_test_results.total_tests);
    printf("Passed:      %d\n", g_test_results.passed_tests);
    printf("Failed:      %d\n", g_test_results.failed_tests);
    printf("Success Rate: %.1f%%\n", 
           g_test_results.total_tests > 0 ? 
           (double)g_test_results.passed_tests / g_test_results.total_tests * 100 : 0);
    for(int i = 0; i < 50; i++) printf("=");
    printf("\n");
}

// 测试数据库初始化和清理
bool test_database_init_cleanup(void) {
    TEST_START("Database Init & Cleanup");
    
    rt_db_handle_t handle;
    
    // 测试初始化
    bool init_result = rt_db_init(&handle, NULL);
    TEST_ASSERT(init_result, "Database initialization");
    
    // 测试句柄有效性
    TEST_ASSERT(handle.shm_addr != NULL, "Valid shared memory address");
    
    // 测试清理
    rt_db_cleanup(&handle);
    TEST_ASSERT(handle.shm_addr == NULL, "Cleanup resets handle");
    
    TEST_END();
}

// 测试数据点查找
bool test_data_point_lookup(rt_db_handle_t* handle) {
    TEST_START("Data Point Lookup");
    
    // 查找已知数据点
    size_t soc_index = rt_db_find_index_by_id(handle, "BMS_01.SOC");
    TEST_ASSERT(soc_index != (size_t)-1, "Find BMS_01.SOC");
    
    size_t power_index = rt_db_find_index_by_id(handle, "PCS_01.Power");
    TEST_ASSERT(power_index != (size_t)-1, "Find PCS_01.Power");
    
    // 查找不存在的数据点
    size_t invalid_index = rt_db_find_index_by_id(handle, "NonExistent.Point");
    TEST_ASSERT(invalid_index == (size_t)-1, "Non-existent point returns -1");
    
    // 测试获取数据点信息
    char point_id[64], units[16];
    bool info_result = rt_db_get_point_info(handle, soc_index, point_id, units);
    TEST_ASSERT(info_result, "Get point info");
    TEST_ASSERT(strcmp(point_id, "BMS_01.SOC") == 0, "Correct point ID");
    
    TEST_END();
}

// 测试数据读写操作
bool test_data_read_write(rt_db_handle_t* handle) {
    TEST_START("Data Read/Write Operations");
    
    size_t test_index = rt_db_find_index_by_id(handle, "BMS_01.SOC");
    TEST_ASSERT(test_index != (size_t)-1, "Find test data point");
    
    // 测试写入数据
    double test_value = 85.5;
    long test_quality = 1; // QUALITY_GOOD
    bool write_result = rt_db_set_value(handle, test_index, test_value, test_quality);
    TEST_ASSERT(write_result, "Write data value");
    
    // 测试读取数据
    double read_value;
    long read_quality;
    struct timespec read_timestamp;
    bool read_result = rt_db_get_value(handle, test_index, &read_value, &read_quality, &read_timestamp);
    TEST_ASSERT(read_result, "Read data value");
    TEST_ASSERT(read_value == test_value, "Read correct value");
    TEST_ASSERT(read_quality == test_quality, "Read correct quality");
    
    // 测试批量写入
    size_t indices[] = {0, 1, 2};
    double values[] = {75.0, 48.2, 12.5};
    long qualities[] = {1, 1, 1};
    bool batch_write = rt_db_set_multiple_values(handle, indices, 3, values, qualities);
    TEST_ASSERT(batch_write, "Batch write operation");
    
    // 测试批量读取
    double read_values[3];
    long read_qualities[3];
    struct timespec read_timestamps[3];
    bool batch_read = rt_db_get_multiple_values(handle, indices, 3, read_values, read_qualities, read_timestamps);
    TEST_ASSERT(batch_read, "Batch read operation");
    
    TEST_END();
}

// 测试指令队列操作
bool test_command_queue(rt_db_handle_t* handle) {
    TEST_START("Command Queue Operations");
    
    // 创建测试指令
    ControlCommand test_cmd = {
        .command_type = 1,
        .value = -50.0,  // 充电50kW
        .priority = 10
    };
    strncpy(test_cmd.device_id, "PCS_01", sizeof(test_cmd.device_id) - 1);
    
    // 测试推送指令
    bool push_result = rt_db_push_command(handle, &test_cmd);
    TEST_ASSERT(push_result, "Push command to queue");
    
    // 测试弹出指令
    ControlCommand read_cmd;
    bool pop_result = rt_db_pop_command(handle, &read_cmd);
    TEST_ASSERT(pop_result, "Pop command from queue");
    TEST_ASSERT(strcmp(read_cmd.device_id, test_cmd.device_id) == 0, "Correct device ID");
    TEST_ASSERT(read_cmd.command_type == test_cmd.command_type, "Correct command type");
    TEST_ASSERT(read_cmd.value == test_cmd.value, "Correct command value");
    
    // 测试空队列弹出
    ControlCommand empty_cmd;
    bool empty_pop = rt_db_pop_command(handle, &empty_cmd);
    TEST_ASSERT(!empty_pop, "Empty queue pop returns false");
    
    TEST_END();
}

// 测试系统状态
bool test_system_status(rt_db_handle_t* handle) {
    TEST_START("System Status Operations");
    
    // 测试获取写计数
    size_t write_count = rt_db_get_write_count(handle);
    TEST_ASSERT(write_count > 0, "Write count > 0 after previous operations");
    
    // 测试获取连接客户端数
    size_t connected_clients = rt_db_get_connected_clients(handle);
    TEST_ASSERT(connected_clients > 0, "Connected clients > 0");
    
    // 测试关闭请求状态
    bool shutdown_requested = rt_db_is_shutdown_requested(handle);
    TEST_ASSERT(!shutdown_requested, "Shutdown not requested initially");
    
    // 测试配置信息
    size_t max_points = rt_db_get_max_data_points();
    TEST_ASSERT(max_points == 10000, "Correct max data points");
    
    size_t queue_size = rt_db_get_command_queue_size();
    TEST_ASSERT(queue_size == 256, "Correct command queue size");
    
    TEST_END();
}

// 性能测试
bool test_performance(rt_db_handle_t* handle) {
    TEST_START("Performance Test");
    
    const int num_operations = 10000;
    size_t test_index = 0;
    
    // 测试写入性能
    clock_t start_time = clock();
    for (int i = 0; i < num_operations; i++) {
        rt_db_set_value(handle, test_index, (double)i, 1);
    }
    clock_t end_time = clock();
    
    double write_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    double write_rate = num_operations / write_time;
    
    printf("Write performance: %.0f ops/sec ", write_rate);
    TEST_ASSERT(write_rate > 1000, "Write rate > 1000 ops/sec");
    
    // 测试读取性能
    start_time = clock();
    for (int i = 0; i < num_operations; i++) {
        double value;
        long quality;
        rt_db_get_value(handle, test_index, &value, &quality, NULL);
    }
    end_time = clock();
    
    double read_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    double read_rate = num_operations / read_time;
    
    printf("Read performance: %.0f ops/sec ", read_rate);
    TEST_ASSERT(read_rate > 10000, "Read rate > 10000 ops/sec");
    
    TEST_END();
}

// 演示实际使用场景
void demo_real_scenario(rt_db_handle_t* handle) {
    printf("\n");
    for(int i = 0; i < 60; i++) printf("=");
    printf("\n");
    printf("REAL SCENARIO DEMONSTRATION\n");
    for(int i = 0; i < 60; i++) printf("=");
    printf("\n");
    
    printf("\\n1. 模拟SCADA系统更新数据:\\n");
    
    // 模拟BMS数据更新
    size_t soc_index = rt_db_find_index_by_id(handle, "BMS_01.SOC");
    size_t voltage_index = rt_db_find_index_by_id(handle, "BMS_01.Voltage");
    size_t current_index = rt_db_find_index_by_id(handle, "BMS_01.Current");
    
    rt_db_set_value(handle, soc_index, 82.5, 1);        // SOC: 82.5%
    rt_db_set_value(handle, voltage_index, 385.2, 1);   // 电压: 385.2V
    rt_db_set_value(handle, current_index, -15.8, 1);   // 电流: -15.8A (充电)
    
    printf("  - BMS_01.SOC = 82.5%%\\n");
    printf("  - BMS_01.Voltage = 385.2V\\n");
    printf("  - BMS_01.Current = -15.8A\\n");
    
    // 模拟PCS数据更新
    size_t power_index = rt_db_find_index_by_id(handle, "PCS_01.Power");
    size_t status_index = rt_db_find_index_by_id(handle, "PCS_01.Status");
    
    rt_db_set_value(handle, power_index, -45.2, 1);     // 功率: -45.2kW (充电)
    rt_db_set_value(handle, status_index, 2, 1);        // 状态: 运行
    
    printf("  - PCS_01.Power = -45.2kW\\n");
    printf("  - PCS_01.Status = 2 (运行)\\n");
    
    printf("\\n2. 算法模块读取数据进行计算:\\n");
    
    double soc, voltage, current, power;
    long quality;
    
    rt_db_get_value(handle, soc_index, &soc, &quality, NULL);
    rt_db_get_value(handle, voltage_index, &voltage, &quality, NULL);
    rt_db_get_value(handle, current_index, &current, &quality, NULL);
    rt_db_get_value(handle, power_index, &power, &quality, NULL);
    
    // 简单的功率计算验证
    double calculated_power = voltage * current / 1000.0; // kW
    printf("  - 计算功率: %.1fkW (实际: %.1fkW)\\n", calculated_power, power);
    
    // 模拟控制算法决策
    if (soc < 90.0 && power < 0) {
        printf("  - 决策: 继续充电 (SOC未满且正在充电)\\n");
    } else {
        printf("  - 决策: 调整功率\\n");
    }
    
    printf("\\n3. 下发控制指令:\\n");
    
    ControlCommand charge_cmd = {
        .command_type = 1,
        .value = -50.0,  // 目标功率 -50kW
        .priority = 10
    };
    strncpy(charge_cmd.device_id, "PCS_01", sizeof(charge_cmd.device_id) - 1);
    
    if (rt_db_push_command(handle, &charge_cmd)) {
        printf("  - 指令已推送: PCS_01 设置功率为 -50kW\\n");
    }
    
    printf("\\n4. SCADA模块获取指令:\\n");
    
    ControlCommand received_cmd;
    if (rt_db_pop_command(handle, &received_cmd)) {
        printf("  - 收到指令: %s 类型=%d 值=%.1f 优先级=%d\\n",
               received_cmd.device_id, received_cmd.command_type, 
               received_cmd.value, received_cmd.priority);
    }
    
    printf("\\n5. 系统状态监控:\\n");
    printf("  - 总写入次数: %zu\\n", rt_db_get_write_count(handle));
    printf("  - 连接客户端: %zu\\n", rt_db_get_connected_clients(handle));
    printf("  - 最大数据点: %zu\\n", rt_db_get_max_data_points());
}

int main(void) {
    printf("Real-Time Database API Test Suite\\n");
    printf("================================\\n");
    printf("Testing RT_DB implementation...\\n");
    
    rt_db_handle_t handle;
    
    // 运行所有测试
    if (!test_database_init_cleanup()) goto cleanup;
    
    // 重新初始化用于后续测试
    if (!rt_db_init(&handle, NULL)) {
        printf("\\nERROR: Failed to initialize database for testing!\\n");
        printf("Please make sure rt_db_init is running.\\n");
        return EXIT_FAILURE;
    }
    
    test_data_point_lookup(&handle);
    test_data_read_write(&handle);
    test_command_queue(&handle);
    test_system_status(&handle);
    test_performance(&handle);
    
    // 运行实际场景演示
    demo_real_scenario(&handle);
    
cleanup:
    rt_db_cleanup(&handle);
    
    // 打印测试结果
    print_test_summary();
    
    if (g_test_results.failed_tests == 0) {
        printf("\\nAll tests passed! The RT_DB system is working correctly.\\n");
        return EXIT_SUCCESS;
    } else {
        printf("\\nSome tests failed. Please check the implementation.\\n");
        return EXIT_FAILURE;
    }

    while (1) {}
}