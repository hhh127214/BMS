#include "../rt_db/rt_db_structs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <conio.h>
#define sleep(x) Sleep((x) * 1000)
static HANDLE g_shared_memory_handle = NULL;
static const char* SHARED_MEMORY_NAME = "RT_DB_SHARED_MEMORY";
static volatile bool g_shutdown_requested = false;
#else
#include <sys/shm.h>
#include <sys/ipc.h>
#include <unistd.h>
static volatile sig_atomic_t g_shutdown_requested = 0;
#endif

// 信号处理函数
#ifndef _WIN32
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        g_shutdown_requested = 1;
        printf("\nReceived shutdown signal, cleaning up...\n");
    }
}
#else
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_shutdown_requested = true;
        printf("\nReceived shutdown signal, cleaning up...\n");
        return TRUE;
    }
    return FALSE;
}
#endif

// Windows平台的共享内存操作
#ifdef _WIN32
int create_shared_memory_segment(void** shm_addr) {
    g_shared_memory_handle = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        sizeof(SharedMemorySegment),
        SHARED_MEMORY_NAME
    );
    
    if (g_shared_memory_handle == NULL) {
        fprintf(stderr, "CreateFileMapping failed: %lu\n", GetLastError());
        return -1;
    }
    
    // 检查是否已存在
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        fprintf(stderr, "Shared memory already exists! Please stop existing instances first.\n");
        CloseHandle(g_shared_memory_handle);
        return -2;
    }
    
    *shm_addr = MapViewOfFile(
        g_shared_memory_handle,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedMemorySegment)
    );
    
    if (*shm_addr == NULL) {
        fprintf(stderr, "MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(g_shared_memory_handle);
        return -1;
    }
    
    return 0;
}

void cleanup_shared_memory(void* shm_addr) {
    if (shm_addr != NULL) {
        UnmapViewOfFile(shm_addr);
    }
    if (g_shared_memory_handle != NULL) {
        CloseHandle(g_shared_memory_handle);
        g_shared_memory_handle = NULL;
    }
}
#endif

// 初始化共享内存段
void initialize_shared_memory_segment(SharedMemorySegment* segment) {
    printf("Initializing shared memory segment...\n");
    
    // 清零整个内存段
    memset(segment, 0, sizeof(SharedMemorySegment));
    
    // 初始化头部信息
    atomic_init(&segment->header.write_count, 0);
    atomic_init(&segment->header.shutdown_requested, false);
    atomic_init(&segment->header.connected_clients, 0);
    
#ifdef _WIN32
    segment->header.manager_pid = _getpid();
#else
    segment->header.manager_pid = getpid();
#endif
    segment->header.num_data_points = MAX_DATA_POINTS;
    
    // 获取当前时间
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER ui;
    GetSystemTimeAsFileTime(&ft);
    ui.LowPart = ft.dwLowDateTime;
    ui.HighPart = ft.dwHighDateTime;
    ui.QuadPart = ui.QuadPart - 116444736000000000ULL;
    segment->header.last_updated.tv_sec = (long)(ui.QuadPart / 10000000ULL);
    segment->header.last_updated.tv_nsec = (long)((ui.QuadPart % 10000000ULL) * 100);
#else
    clock_gettime(CLOCK_REALTIME, &segment->header.last_updated);
#endif
    
    // 初始化数据点
    printf("Initializing %d data points...\n", MAX_DATA_POINTS);
    for (size_t i = 0; i < MAX_DATA_POINTS; i++) {
        snprintf(segment->data_points[i].point_id, MAX_POINT_ID_LEN, "UNUSED_%zu", i);
        atomic_init(&segment->data_points[i].value, 0.0);
        atomic_init(&segment->data_points[i].quality, QUALITY_BAD);
        atomic_init(&segment->data_points[i].sequence, 0);
        strcpy(segment->data_points[i].units, "");
        segment->data_points[i].timestamp = segment->header.last_updated;
    }
    
    // 初始化指令队列
    atomic_init(&segment->command_queue.head, 0);
    atomic_init(&segment->command_queue.tail, 0);
    
    printf("Shared memory initialization completed successfully.\n");
}

// 监控共享内存状态
void monitor_shared_memory(SharedMemorySegment* segment) {
    size_t last_write_count = 0;
    int monitor_interval = 5; // 监控间隔（秒）
    
    printf("Starting monitoring (interval: %d seconds)...\n", monitor_interval);
    printf("Press Ctrl+C to shutdown gracefully.\n\n");
    
    while (!g_shutdown_requested && 
           !atomic_load_explicit(&segment->header.shutdown_requested, memory_order_relaxed)) {
        
        // 获取当前状态
        size_t current_write_count = atomic_load_explicit(&segment->header.write_count, memory_order_relaxed);
        int connected_clients = atomic_load_explicit(&segment->header.connected_clients, memory_order_relaxed);
        
        // 计算写入速率
        size_t writes_per_interval = current_write_count - last_write_count;
        double writes_per_second = (double)writes_per_interval / monitor_interval;
        
        // 输出状态信息
        printf("\\r[Status] Clients: %d, Total Writes: %zu, Rate: %.1f writes/sec", 
               connected_clients, current_write_count, writes_per_second);
        fflush(stdout);
        
        last_write_count = current_write_count;
        
        // 等待下一个监控周期
        for (int i = 0; i < monitor_interval && !g_shutdown_requested; i++) {
            sleep(1);
        }
    }
    
    printf("\n\nShutdown requested, setting shutdown flag...\n");
    atomic_store_explicit(&segment->header.shutdown_requested, true, memory_order_relaxed);
}

int main(int argc, char* argv[]) {
    (void)argc; // 避免未使用参数警告
    (void)argv; // 避免未使用参数警告
    
    printf("Real-Time Database Shared Memory Initializer\n");
    printf("============================================\n\n");
    
    // 设置信号处理
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
#endif
    
    void* shm_addr = NULL;
    int shm_id = -1;  // 在这里统一声明，避免重复声明
    
#ifdef _WIN32
    // Windows平台初始化
    if (create_shared_memory_segment(&shm_addr) != 0) {
        return EXIT_FAILURE;
    }
    printf("Shared memory created successfully (Windows File Mapping)\n");
#else
    // Linux平台初始化
    key_t key = ftok("/tmp", 'R');
    if (key == -1) {
        perror("ftok failed");
        return EXIT_FAILURE;
    }
    
    shm_id = shmget(key, sizeof(SharedMemorySegment), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_id == -1) {
        if (errno == EEXIST) {
            fprintf(stderr, "Shared memory already exists! Please stop existing instances first.\n");
            fprintf(stderr, "Or remove it manually with: ipcrm -M %d\n", key);
        } else {
            perror("shmget failed");
        }
        return EXIT_FAILURE;
    }
    
    shm_addr = shmat(shm_id, NULL, 0);
    if (shm_addr == (void*)-1) {
        perror("shmat failed");
        shmctl(shm_id, IPC_RMID, NULL);
        return EXIT_FAILURE;
    }
    
    printf("Shared memory created successfully (SHM ID: %d, Key: 0x%x)\n", shm_id, key);
#endif
    
    // 初始化共享内存段
    SharedMemorySegment* segment = (SharedMemorySegment*)shm_addr;
    initialize_shared_memory_segment(segment);
    
    // 开始监控
    monitor_shared_memory(segment);
    
    // 清理资源
    printf("Cleaning up shared memory...\n");
    
#ifdef _WIN32
    cleanup_shared_memory(shm_addr);
#else
    shmdt(shm_addr);
    shmctl(shm_id, IPC_RMID, NULL);
#endif
    
    printf("Cleanup completed. Goodbye!\n");
    return EXIT_SUCCESS;
}