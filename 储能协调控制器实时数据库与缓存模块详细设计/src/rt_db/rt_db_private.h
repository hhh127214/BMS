#ifndef RT_DB_PRIVATE_H
#define RT_DB_PRIVATE_H

#include "rt_db_structs.h"
#include <stdbool.h>

// 前置声明
typedef struct rt_db_handle rt_db_handle_t;

// 内部函数声明
bool build_index_map(rt_db_handle_t* handle, const char* config_path);
size_t next_index(size_t current, size_t max);

// 共享内存相关内部函数
#ifdef _WIN32
// Windows特定的共享内存函数
int get_shm_key(void);
int create_shared_memory(size_t size);
int open_shared_memory(void);
void* map_shared_memory(int shm_id, size_t size);
bool unmap_shared_memory(void* addr, size_t size);
bool destroy_shared_memory(int shm_id);
#else
// Linux特定的共享内存函数
key_t get_shm_key(void);
#endif

// 内部工具函数
bool validate_handle(const rt_db_handle_t* handle);
bool validate_index(size_t index);
void update_timestamp(struct timespec* ts);

#endif // RT_DB_PRIVATE_H