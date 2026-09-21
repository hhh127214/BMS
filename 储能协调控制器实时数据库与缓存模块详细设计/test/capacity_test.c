#include "../include/rt_db_api.h"
#include "../include/rt_db_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define sleep(x) Sleep((x) * 1000)
#else
#include <unistd.h>
#endif

// 内存容量分析函数
void analyze_memory_usage(void) {
    printf("=======================================================\n");
    printf("          实时数据库内存容量分析报告\n");
    printf("=======================================================\n\n");
    
    printf("📋 当前配置参数:\n");
    printf("  - 最大数据点数量:    %d 个\n", rt_db_get_max_data_points());
    printf("  - 指令队列大小:      %d 个指令\n", rt_db_get_command_queue_size());
    
    // 计算不同容量下的内存需求
    printf("\n💾 不同容量配置的内存需求估算:\n");
    int capacities[] = {1000, 5000, 10000, 20000, 50000, 100000};
    int capacity_count = sizeof(capacities) / sizeof(capacities[0]);
    
    for (int i = 0; i < capacity_count; i++) {
        // 简单估算：每个数据点大约128字节
        size_t estimated_size = 128 * capacities[i] / (1024 * 1024);
        printf("  - %6d 数据点: ~%zu MB\n", capacities[i], estimated_size);
    }
    
    printf("\n");
}

// 压力测试函数
bool stress_test_capacity(int target_points) {
    printf("🧪 开始容量压力测试 (目标: %d 数据点)\n", target_points);
    
    rt_db_handle_t handle;
    if (!rt_db_init(&handle, NULL)) {
        printf("❌ 数据库连接失败\n");
        return false;
    }
    
    printf("✅ 数据库连接成功\n");
    
    // 测试写入所有可用数据点
    int success_count = 0;
    int max_test_points = (target_points > rt_db_get_max_data_points()) ? rt_db_get_max_data_points() : target_points;
    
    printf("📝 写入测试开始...\n");
    for (int i = 0; i < max_test_points; i++) {
        double test_value = 100.0 + (double)i * 0.1;
        if (rt_db_set_value(&handle, i, test_value, 1)) {
            success_count++;
        } else {
            printf("❌ 数据点 %d 写入失败\n", i);
            break;
        }
        
        // 每1000个点显示进度
        if ((i + 1) % 1000 == 0) {
            printf("   进度: %d/%d 数据点已写入\n", i + 1, max_test_points);
        }
    }
    
    printf("📊 写入测试结果: %d/%d 数据点成功写入\n", success_count, max_test_points);
    
    // 验证数据读取
    printf("📖 读取验证开始...\n");
    int read_success = 0;
    int read_errors = 0;
    
    for (int i = 0; i < success_count && i < 100; i++) {  // 只验证前100个
        double value;
        long quality;
        struct timespec timestamp;
        
        if (rt_db_get_value(&handle, i, &value, &quality, &timestamp)) {
            double expected = 100.0 + (double)i * 0.1;
            if (value == expected && quality == 1) {
                read_success++;
            } else {
                read_errors++;
                if (read_errors < 10) {  // 只显示前10个错误
                    printf("❌ 数据点 %d 验证失败: 期望 %.1f, 实际 %.1f\n", 
                           i, expected, value);
                }
            }
        } else {
            read_errors++;
        }
    }
    
    printf("📊 读取验证结果: %d 成功, %d 错误\n", read_success, read_errors);
    
    // 性能测试
    printf("⚡ 性能测试开始...\n");
    clock_t start_time = clock();
    int perf_operations = 1000;
    
    for (int i = 0; i < perf_operations; i++) {
        size_t index = i % success_count;
        double value;
        long quality;
        rt_db_get_value(&handle, index, &value, &quality, NULL);
    }
    
    clock_t end_time = clock();
    double elapsed = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    double ops_per_sec = perf_operations / elapsed;
    
    printf("📊 性能测试结果: %.0f ops/sec\n", ops_per_sec);
    
    // 系统状态检查
    size_t write_count = rt_db_get_write_count(&handle);
    size_t connected_clients = rt_db_get_connected_clients(&handle);
    
    printf("📊 系统状态:\n");
    printf("   - 总写入次数: %zu\n", write_count);
    printf("   - 连接客户端: %zu\n", connected_clients);
    
    rt_db_cleanup(&handle);
    
    bool test_passed = (success_count == max_test_points) && (read_errors == 0);
    printf("%s 容量测试 %s\n", 
           test_passed ? "✅" : "❌", 
           test_passed ? "通过" : "失败");
    
    return test_passed;
}

// 指令队列容量测试
void test_command_queue_capacity(void) {
    printf("\n🔧 指令队列容量测试\n");
    printf("==================\n");
    
    rt_db_handle_t handle;
    if (!rt_db_init(&handle, NULL)) {
        printf("❌ 数据库连接失败\n");
        return;
    }
    
    // 测试指令队列容量
    int push_count = 0;
    printf("📝 推送指令测试...\n");
    
    for (int i = 0; i < rt_db_get_command_queue_size() + 100; i++) {  // 超过队列大小
        ControlCommand cmd = {
            .command_type = 1,
            .value = (double)i,
            .priority = 10
        };
        snprintf(cmd.device_id, sizeof(cmd.device_id), "DEVICE_%03d", i);
        
        if (rt_db_push_command(&handle, &cmd)) {
            push_count++;
        } else {
            printf("   队列已满，停止推送 (已推送 %d 条指令)\n", push_count);
            break;
        }
    }
    
    printf("📊 推送结果: %d/%d 指令成功推送\n", push_count, (int)rt_db_get_command_queue_size());
    
    // 弹出测试
    int pop_count = 0;
    printf("📖 弹出指令测试...\n");
    
    ControlCommand cmd;
    while (rt_db_pop_command(&handle, &cmd)) {
        pop_count++;
    }
    
    printf("📊 弹出结果: %d 条指令成功弹出\n", pop_count);
    printf("%s 指令队列测试 %s\n", 
           (push_count <= (int)rt_db_get_command_queue_size() && pop_count == push_count) ? "✅" : "❌",
           (push_count <= (int)rt_db_get_command_queue_size() && pop_count == push_count) ? "通过" : "失败");
    
    rt_db_cleanup(&handle);
}

int main(void) {
    printf("实时数据库容量测试程序\n");
    printf("===================\n\n");
    
    // 分析当前内存使用
    analyze_memory_usage();
    
    // 进行容量压力测试
    printf("🚀 开始容量压力测试...\n");
    printf("===================\n");
    
    // 测试当前配置的容量
    stress_test_capacity(rt_db_get_max_data_points());
    
    // 测试指令队列容量
    test_command_queue_capacity();
    
    printf("\n💡 建议:\n");
    printf("========\n");
    printf("1. 当前配置 (%d 数据点) 内存使用约 %.2f MB\n", 
           (int)rt_db_get_max_data_points(),
           (double)(128 * rt_db_get_max_data_points()) / (1024.0 * 1024.0));
    printf("2. 如需增加容量，可修改 MAX_DATA_POINTS 常量\n");
    printf("3. 建议根据实际硬件内存限制和性能需求调整\n");
    printf("4. 大容量配置建议进行充分测试\n\n");
    
    printf("🎯 测试完成！\n");
    return 0;
}