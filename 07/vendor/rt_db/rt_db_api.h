#ifndef RT_DB_API_H
#define RT_DB_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <time.h>
#include <stddef.h>

// 控制指令结构定义
#ifndef CONTROLCOMMAND_DEFINED
#define CONTROLCOMMAND_DEFINED
typedef struct ControlCommand {
    char device_id[64];         // 目标设备ID
    int command_type;           // 指令类型
    double value;               // 指令值
    int priority;               // 优先级
    struct timespec issue_time; // 下发时间
} ControlCommand;
#endif

// 数据库连接句柄
typedef struct rt_db_handle {
    int shm_id;                 // 共享内存ID
    void* shm_addr;             // 共享内存映射地址
    size_t* point_index_map;    // 点ID到索引的映射表
    size_t map_size;            // 映射表大小
} rt_db_handle_t;

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

// --- 共享内存底层原语（供 ems_rt_db_setup.c 调用）---
// ⚠ 必须在此声明：否则调用方按"隐式 int"处理其返回值，64 位指针被截断成 32 位，
//    在 aarch64 mmap 高位地址（bit31=1）上符号扩展成非法地址 → memset SIGSEGV。
//    Linux=SysV shm，Windows=文件映射，两侧 rt_db_api.c 均已实现，声明可无条件放这里。
int   get_shm_key(void);
int   create_shared_memory(size_t size);
int   open_shared_memory(void);
void* map_shared_memory(int shm_id, size_t size);
bool  unmap_shared_memory(void* addr, size_t size);
bool  destroy_shared_memory(int shm_id);

#ifdef __cplusplus
}
#endif

#endif // RT_DB_API_H