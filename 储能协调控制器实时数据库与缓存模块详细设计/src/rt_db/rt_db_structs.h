#ifndef RT_DB_STRUCTS_H
#define RT_DB_STRUCTS_H

#include <stdbool.h>
#include <time.h>
#include <stddef.h>

// 原子操作兼容性处理
#ifdef _WIN32
#include <windows.h>
#include <intrin.h>

// Windows下的原子类型定义
typedef volatile LONG64 atomic_size_t;
typedef volatile LONG atomic_bool;
// Windows下double原子操作使用互斥保护，这里先用volatile double
typedef volatile double atomic_double;
typedef volatile LONG64 atomic_long;
typedef volatile LONG atomic_int;

// 内存序定义（简化版）
typedef enum {
    memory_order_relaxed,
    memory_order_acquire,
    memory_order_release,
    memory_order_acq_rel,
    memory_order_seq_cst
} memory_order;

// Windows原子操作函数宏
#define atomic_load_explicit(ptr, order) (*(ptr))
#define atomic_store_explicit(ptr, val, order) (*(ptr) = (val))
#define atomic_fetch_add_explicit(ptr, val, order) \
    ((sizeof(*(ptr)) == sizeof(LONG)) ? \
     InterlockedAdd((LONG*)(ptr), (LONG)(val)) : \
     InterlockedAdd64((LONG64*)(ptr), (LONG64)(val)))
#define atomic_fetch_sub_explicit(ptr, val, order) \
    ((sizeof(*(ptr)) == sizeof(LONG)) ? \
     InterlockedAdd((LONG*)(ptr), -(LONG)(val)) : \
     InterlockedAdd64((LONG64*)(ptr), -(LONG64)(val)))
#define atomic_thread_fence(order) MemoryBarrier()
#define atomic_init(ptr, val) (*(ptr) = (val))

#else
// Linux/Unix平台使用标准C11原子操作
#include <stdatomic.h>

// 标准原子类型重定义
typedef _Atomic size_t atomic_size_t;
typedef _Atomic bool atomic_bool;
typedef _Atomic double atomic_double;
typedef _Atomic long atomic_long;
typedef _Atomic int atomic_int;

#endif

#ifdef _WIN32
#include <process.h>
typedef int pid_t;
#else
#include <sys/types.h>
#endif

// --- 配置常量 ---
#define MAX_DATA_POINTS  20000   // 最大支持数据点数量（覆盖储能7300、光伏10000、风电11000系列指令输出）
#define MAX_POINT_ID_LEN 64
#define MAX_UNIT_LEN     16
#define COMMAND_QUEUE_SIZE 256

// --- 数据质量定义 ---
typedef enum {
    QUALITY_BAD = 0,
    QUALITY_GOOD = 1,
    QUALITY_UNCERTAIN = 2
} data_quality_t;

// --- 元数据区结构 ---
typedef struct {
    atomic_size_t write_count;         // 写计数器，用于乐观锁
    atomic_bool shutdown_requested;    // 优雅关闭标志
    struct timespec last_updated;      // 最后更新时间戳
    size_t num_data_points;            // 实际数据点数量
    atomic_int connected_clients;      // 当前连接的客户端数量
    pid_t manager_pid;                 // 管理进程PID
} SharedMemoryHeader;

// --- 实时数据点结构 ---
typedef struct {
    atomic_double value;               // 数据值
    atomic_long quality;               // 数据质量
    atomic_long sequence;              // 序列号，用于检测数据更新
    struct timespec timestamp;         // 数据时间戳
    char point_id[MAX_POINT_ID_LEN];   // 数据点ID
    char units[MAX_UNIT_LEN];          // 单位
} DataPoint;

// --- 控制指令结构 ---
#ifndef CONTROLCOMMAND_DEFINED
#define CONTROLCOMMAND_DEFINED
typedef struct ControlCommand {
    char device_id[MAX_POINT_ID_LEN];  // 目标设备ID
    int command_type;                  // 指令类型
    double value;                      // 指令值
    int priority;                      // 优先级
    struct timespec issue_time;        // 下发时间
} ControlCommand;
#endif

// --- 无锁环形指令队列 ---
typedef struct {
    atomic_size_t head;                // 写索引
    atomic_size_t tail;                // 读索引
    ControlCommand commands[COMMAND_QUEUE_SIZE];
} CommandRingBuffer;

// --- 共享内存整体布局 ---
typedef struct {
    SharedMemoryHeader header;
    DataPoint data_points[MAX_DATA_POINTS];
    CommandRingBuffer command_queue;
} SharedMemorySegment;

#endif // RT_DB_STRUCTS_H