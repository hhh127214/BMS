#ifndef RT_DB_API_H
#define RT_DB_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <time.h>
#include <stddef.h>

// 前置声明，避免包含内部结构
typedef struct ControlCommand ControlCommand;

// 数据库连接句柄
typedef struct rt_db_handle {
    int shm_id;                 // 共享内存ID
    void* shm_addr;             // 共享内存映射地址
    size_t* point_index_map;    // 点ID到索引的映射表
    size_t map_size;            // 映射表大小
} rt_db_handle_t;

// 控制指令结构定义（对外可见）
typedef struct ControlCommand {
    char device_id[64];         // 目标设备ID
    int command_type;           // 指令类型
    double value;               // 指令值
    int priority;               // 优先级
    struct timespec issue_time; // 下发时间
} ControlCommand;

// --- 初始化与清理 ---
bool rt_db_init(rt_db_handle_t* handle, const char* config_path);
void rt_db_cleanup(rt_db_handle_t* handle);

// --- 数据点操作 ---
bool rt_db_get_value(const rt_db_handle_t* handle, size_t index, double* value, long* quality, struct timespec* timestamp);
bool rt_db_set_value(rt_db_handle_t* handle, size_t index, double value, long quality);

// --- 数据点查询 ---
size_t rt_db_find_index_by_id(const rt_db_handle_t* handle, const char* point_id);
bool rt_db_get_point_info(const rt_db_handle_t* handle, size_t index, char* point_id, char* units);

// --- 指令队列操作 ---
bool rt_db_push_command(rt_db_handle_t* handle, const ControlCommand* command);
bool rt_db_pop_command(rt_db_handle_t* handle, ControlCommand* command);

// --- 系统状态 ---
bool rt_db_is_shutdown_requested(const rt_db_handle_t* handle);
size_t rt_db_get_write_count(const rt_db_handle_t* handle);
size_t rt_db_get_connected_clients(const rt_db_handle_t* handle);

// --- 批量操作 ---
bool rt_db_get_multiple_values(const rt_db_handle_t* handle, const size_t* indices, size_t count, 
                               double* values, long* qualities, struct timespec* timestamps);
bool rt_db_set_multiple_values(rt_db_handle_t* handle, const size_t* indices, size_t count,
                               const double* values, const long* qualities);

// --- 配置管理 ---
size_t rt_db_get_max_data_points(void);
size_t rt_db_get_command_queue_size(void);

#ifdef __cplusplus
}
#endif

#endif // RT_DB_API_H