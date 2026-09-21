#include "rt_db_api.h"
#include "rt_db_api.h"
#include "rt_db_structs.h"
#include "rt_db_private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#define snprintf _snprintf
#define strcasecmp _stricmp
static HANDLE g_shared_memory_handle = NULL;
static const char* SHARED_MEMORY_NAME = "RT_DB_SHARED_MEMORY";
#else
#include <sys/shm.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// 全局映射表（用于点ID查找）
static struct {
    char point_id[MAX_POINT_ID_LEN];
    size_t index;
} g_point_map[MAX_DATA_POINTS];
static size_t g_point_map_size = 0;

// =============================================================================
// 内部工具函数实现
// =============================================================================

bool validate_handle(const rt_db_handle_t* handle) {
    return (handle != NULL && handle->shm_addr != NULL);
}

bool validate_index(size_t index) {
    return (index < MAX_DATA_POINTS);
}

void update_timestamp(struct timespec* ts) {
#ifdef _WIN32
    // Windows平台获取高精度时间戳
    FILETIME ft;
    ULARGE_INTEGER ui;
    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    
    // 转换为Unix时间戳（纳秒）
    ui.QuadPart = ui.QuadPart - 116444736000000000ULL; // 从1601年到1970年的时间差
    ts->tv_sec = (long)(ui.QuadPart / 10000000ULL);
    ts->tv_nsec = (long)((ui.QuadPart % 10000000ULL) * 100);
#else
    clock_gettime(CLOCK_REALTIME, ts);
#endif
}

size_t next_index(size_t current, size_t max) {
    return (current + 1) % max;
}

// =============================================================================
// 共享内存管理函数
// =============================================================================

#ifdef _WIN32
int create_shared_memory(size_t size) {
    g_shared_memory_handle = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        (DWORD)size,
        SHARED_MEMORY_NAME
    );
    
    if (g_shared_memory_handle == NULL) {
        return -1;
    }
    
    // 检查是否已存在
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_shared_memory_handle);
        g_shared_memory_handle = NULL;
        return -2; // 表示已存在
    }
    
    return 0; // 成功创建
}

int open_shared_memory(void) {
    g_shared_memory_handle = OpenFileMapping(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        SHARED_MEMORY_NAME
    );
    
    return (g_shared_memory_handle != NULL) ? 0 : -1;
}

void* map_shared_memory(int shm_id, size_t size) {
    (void)shm_id; // 在Windows下不使用
    if (g_shared_memory_handle == NULL) {
        return NULL;
    }
    
    return MapViewOfFile(
        g_shared_memory_handle,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        size
    );
}

bool unmap_shared_memory(void* addr, size_t size) {
    (void)size; // Windows下不需要size参数
    return UnmapViewOfFile(addr) != 0;
}

bool destroy_shared_memory(int shm_id) {
    (void)shm_id; // Windows下不使用
    if (g_shared_memory_handle != NULL) {
        bool result = CloseHandle(g_shared_memory_handle) != 0;
        g_shared_memory_handle = NULL;
        return result;
    }
    return true;
}

int get_shm_key(void) {
    return 0; // Windows下不使用
}

#else
// Linux实现
key_t get_shm_key(void) {
    return ftok("/tmp", 'R');
}
#endif

// =============================================================================
// 映射表构建函数
// =============================================================================

bool build_index_map(rt_db_handle_t* handle, const char* config_path) {
    (void)config_path; // 添加这一行来避免未使用参数警告
    
    if (!validate_handle(handle)) {
        return false;
    }
    
    // 简化实现：直接从共享内存中构建映射表
    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    g_point_map_size = 0;
    
    for (size_t i = 0; i < MAX_DATA_POINTS && g_point_map_size < MAX_DATA_POINTS; i++) {
        if (strlen(segment->data_points[i].point_id) > 0 && 
            strncmp(segment->data_points[i].point_id, "UNUSED_", 7) != 0) {
            
            strncpy(g_point_map[g_point_map_size].point_id, 
                    segment->data_points[i].point_id, 
                    MAX_POINT_ID_LEN - 1);
            g_point_map[g_point_map_size].point_id[MAX_POINT_ID_LEN - 1] = '\0';
            g_point_map[g_point_map_size].index = i;
            g_point_map_size++;
        }
    }
    
    // 为演示目的，添加一些默认的数据点
    if (g_point_map_size == 0) {
        const char* default_points[] = {
            "BMS_01.SOC", "BMS_01.Voltage", "BMS_01.Current",
            "PCS_01.Power", "PCS_01.Status", "PCS_01.Frequency",
            "SCADA.SystemStatus", "SCADA.TotalPower"
        };
        
        size_t default_count = sizeof(default_points) / sizeof(default_points[0]);
        for (size_t i = 0; i < default_count && i < MAX_DATA_POINTS; i++) {
            strncpy(g_point_map[i].point_id, default_points[i], MAX_POINT_ID_LEN - 1);
            g_point_map[i].point_id[MAX_POINT_ID_LEN - 1] = '\0';
            g_point_map[i].index = i;
            
            // 同时更新共享内存中的点ID
            strncpy(segment->data_points[i].point_id, default_points[i], MAX_POINT_ID_LEN - 1);
            segment->data_points[i].point_id[MAX_POINT_ID_LEN - 1] = '\0';
            strcpy(segment->data_points[i].units, "%");  // 默认单位
        }
        g_point_map_size = default_count;
    }
    
    printf("Built index map with %zu data points\n", g_point_map_size);
    return true;
}

// =============================================================================
// 主要API函数实现
// =============================================================================

bool rt_db_init(rt_db_handle_t* handle, const char* config_path) {
    if (handle == NULL) {
        return false;
    }

    memset(handle, 0, sizeof(rt_db_handle_t));

#ifdef _WIN32
    // Windows实现
    if (open_shared_memory() != 0) {
        fprintf(stderr, "Shared memory not found. Please run init tool first.\n");
        return false;
    }

    void* shm_addr = map_shared_memory(0, sizeof(SharedMemorySegment));
    if (shm_addr == NULL) {
        fprintf(stderr, "Failed to map shared memory\n");
        return false;
    }

    handle->shm_id = 0; // Windows下不使用
    handle->shm_addr = shm_addr;
#else
    // Linux实现
    key_t shm_key = get_shm_key();
    int shm_id = shmget(shm_key, sizeof(SharedMemorySegment), 0666);
    if (shm_id == -1) {
        if (errno == ENOENT) {
            fprintf(stderr, "Shared memory not found. Please run init tool first.\n");
            return false;
        }
        perror("shmget failed");
        return false;
    }

    void* shm_addr = shmat(shm_id, NULL, 0);
    if (shm_addr == (void*)-1) {
        perror("shmat failed");
        return false;
    }

    handle->shm_id = shm_id;
    handle->shm_addr = shm_addr;
#endif

    // 构建点ID到索引的映射表
    if (!build_index_map(handle, config_path)) {
        fprintf(stderr, "Failed to build index map\n");
        rt_db_cleanup(handle);
        return false;
    }

    // 增加连接客户端计数
    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    atomic_fetch_add_explicit(&segment->header.connected_clients, 1, memory_order_relaxed);

    return true;
}

void rt_db_cleanup(rt_db_handle_t* handle) {
    if (handle && handle->shm_addr != NULL) {
        SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
        atomic_fetch_sub_explicit(&segment->header.connected_clients, 1, memory_order_relaxed);
        
#ifdef _WIN32
        unmap_shared_memory(handle->shm_addr, sizeof(SharedMemorySegment));
#else
        shmdt(handle->shm_addr);
#endif
        
        if (handle->point_index_map) {
            free(handle->point_index_map);
        }
        memset(handle, 0, sizeof(rt_db_handle_t));
    }
}

bool rt_db_get_value(const rt_db_handle_t* handle, size_t index, 
                     double* value, long* quality, struct timespec* timestamp) {
    if (!validate_handle(handle) || !validate_index(index)) {
        return false;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    DataPoint* point = &segment->data_points[index];

    // 原子读取序列号，确保数据一致性
    long seq_before = (long)atomic_load_explicit(&point->sequence, memory_order_acquire);
    
    if (value) *value = atomic_load_explicit(&point->value, memory_order_relaxed);
    if (quality) *quality = (long)atomic_load_explicit(&point->quality, memory_order_relaxed);
    if (timestamp) *timestamp = point->timestamp;

    // 检查读取过程中数据是否被修改
    long seq_after = (long)atomic_load_explicit(&point->sequence, memory_order_acquire);
    return (seq_before == seq_after) && (seq_before % 2 == 0); // 序列号为偶数表示数据稳定
}

bool rt_db_set_value(rt_db_handle_t* handle, size_t index, double value, long quality) {
    if (!validate_handle(handle) || !validate_index(index)) {
        return false;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    DataPoint* point = &segment->data_points[index];

    // 增加序列号（变为奇数），表示数据正在更新
    atomic_fetch_add_explicit(&point->sequence, 1, memory_order_relaxed);

    // 内存屏障，确保上面的写操作先于下面的写操作
    atomic_thread_fence(memory_order_release);

    // 更新数据
    atomic_store_explicit(&point->value, value, memory_order_relaxed);
    atomic_store_explicit(&point->quality, quality, memory_order_relaxed);
    update_timestamp(&point->timestamp);

    // 内存屏障，确保数据更新完成后再增加序列号
    atomic_thread_fence(memory_order_release);
    atomic_fetch_add_explicit(&point->sequence, 1, memory_order_relaxed); // 变为偶数，数据可用

    // 更新全局元数据
    atomic_fetch_add_explicit(&segment->header.write_count, 1, memory_order_relaxed);
    update_timestamp(&segment->header.last_updated);

    return true;
}

size_t rt_db_find_index_by_id(const rt_db_handle_t* handle, const char* point_id) {
    if (!validate_handle(handle) || point_id == NULL) {
        return (size_t)-1;
    }

    for (size_t i = 0; i < g_point_map_size; i++) {
        if (strcmp(g_point_map[i].point_id, point_id) == 0) {
            return g_point_map[i].index;
        }
    }

    return (size_t)-1; // 未找到
}

bool rt_db_get_point_info(const rt_db_handle_t* handle, size_t index, 
                          char* point_id, char* units) {
    if (!validate_handle(handle) || !validate_index(index)) {
        return false;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    DataPoint* point = &segment->data_points[index];

    if (point_id) {
        strncpy(point_id, point->point_id, MAX_POINT_ID_LEN - 1);
        point_id[MAX_POINT_ID_LEN - 1] = '\0';
    }

    if (units) {
        strncpy(units, point->units, MAX_UNIT_LEN - 1);
        units[MAX_UNIT_LEN - 1] = '\0';
    }

    return true;
}

// =============================================================================
// 指令队列操作实现
// =============================================================================

bool rt_db_push_command(rt_db_handle_t* handle, const ControlCommand* command) {
    if (!validate_handle(handle) || command == NULL) {
        return false;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    CommandRingBuffer* queue = &segment->command_queue;

    size_t current_head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    size_t next_head = next_index(current_head, COMMAND_QUEUE_SIZE);
    size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_acquire);

    // 检查队列是否已满
    if (next_head == current_tail) {
        return false; // 队列满，推送失败
    }

    // 写入命令
    queue->commands[current_head] = *command;
    update_timestamp(&queue->commands[current_head].issue_time);

    // 更新head指针
    atomic_store_explicit(&queue->head, next_head, memory_order_release);
    return true;
}

bool rt_db_pop_command(rt_db_handle_t* handle, ControlCommand* command) {
    if (!validate_handle(handle) || command == NULL) {
        return false;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    CommandRingBuffer* queue = &segment->command_queue;

    size_t current_tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    size_t current_head = atomic_load_explicit(&queue->head, memory_order_acquire);

    // 检查队列是否为空
    if (current_tail == current_head) {
        return false; // 队列空，弹出失败
    }

    // 读取命令
    *command = queue->commands[current_tail];

    // 更新tail指针
    atomic_store_explicit(&queue->tail, next_index(current_tail, COMMAND_QUEUE_SIZE), memory_order_release);
    return true;
}

// =============================================================================
// 系统状态操作实现
// =============================================================================

bool rt_db_is_shutdown_requested(const rt_db_handle_t* handle) {
    if (!validate_handle(handle)) {
        return false;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    return atomic_load_explicit(&segment->header.shutdown_requested, memory_order_relaxed);
}

size_t rt_db_get_write_count(const rt_db_handle_t* handle) {
    if (!validate_handle(handle)) {
        return 0;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    return atomic_load_explicit(&segment->header.write_count, memory_order_relaxed);
}

size_t rt_db_get_connected_clients(const rt_db_handle_t* handle) {
    if (!validate_handle(handle)) {
        return 0;
    }

    SharedMemorySegment* segment = (SharedMemorySegment*)handle->shm_addr;
    return (size_t)atomic_load_explicit(&segment->header.connected_clients, memory_order_relaxed);
}

// =============================================================================
// 批量操作实现
// =============================================================================

bool rt_db_get_multiple_values(const rt_db_handle_t* handle, const size_t* indices, size_t count,
                               double* values, long* qualities, struct timespec* timestamps) {
    if (!validate_handle(handle) || indices == NULL || count == 0) {
        return false;
    }

    bool all_success = true;
    for (size_t i = 0; i < count; i++) {
        double* value_ptr = values ? &values[i] : NULL;
        long* quality_ptr = qualities ? &qualities[i] : NULL;
        struct timespec* ts_ptr = timestamps ? &timestamps[i] : NULL;
        
        if (!rt_db_get_value(handle, indices[i], value_ptr, quality_ptr, ts_ptr)) {
            all_success = false;
        }
    }

    return all_success;
}

bool rt_db_set_multiple_values(rt_db_handle_t* handle, const size_t* indices, size_t count,
                               const double* values, const long* qualities) {
    if (!validate_handle(handle) || indices == NULL || values == NULL || 
        qualities == NULL || count == 0) {
        return false;
    }

    bool all_success = true;
    for (size_t i = 0; i < count; i++) {
        if (!rt_db_set_value(handle, indices[i], values[i], qualities[i])) {
            all_success = false;
        }
    }

    return all_success;
}

// =============================================================================
// 配置管理实现
// =============================================================================

size_t rt_db_get_max_data_points(void) {
    return MAX_DATA_POINTS;
}

size_t rt_db_get_command_queue_size(void) {
    return COMMAND_QUEUE_SIZE;
}