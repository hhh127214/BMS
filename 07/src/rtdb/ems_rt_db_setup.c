// =====================================================================
// EMS 点表初始化器（补 RT_DB 的一个真实缺陷）
//
// 缺陷：RT_DB 的 build_index_map() 里写着 `(void)config_path;` ——
//       配置参数被直接丢弃，因此**没有任何公开 API 能往共享内存里注册点**。
//       它唯一的兜底是"若一个点都没有，就注入 8 个 demo 点"
//       （BMS_01.SOC / PCS_01.Power / SCADA.* 等），这显然不能用于生产。
//
// 本文件补上这一步：创建（或打开）共享内存 → 写入 EMS 的 30 个点 → 收工。
// 与官方 `src/tools/init_rt_db.c` 的区别：
//   · 官方版是**常驻进程**（跑 monitor 循环，Ctrl+C 才退出），无法用于自动化测试；
//     本版**写完即退出**，可被测试直接调用。
//   · 官方版把所有点初始化为 UNUSED_<i>，本版写入真实点表。
//   · 官方版不注册任何业务点，本版按 ems_point_table.h 注册。
//
// 编译（MinGW）：
//   gcc -std=c11 -c -I. -I../../vendor/rt_db ems_rt_db_setup.c
// =====================================================================

#include "ems_point_table.h"

#include "../../vendor/rt_db/rt_db_api.h"
#include "../../vendor/rt_db/rt_db_private.h"
#include "../../vendor/rt_db/rt_db_structs.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------
// 共享内存「存活句柄」
//
// Windows 的段是**页面文件支撑的文件映射对象**：最后一个句柄关闭即销毁。
// 而本初始化器的语义是"写完就撒手"（不能像官方 init_rt_db.c 那样常驻做
// monitor 循环），如果这里也把句柄关掉，调用方随后的 rt_db_init() 必然报
//   "Shared memory not found. Please run init tool first."
// —— 这正是接入 RT_DB 时踩到的第一个坑（Linux 下 shmget 的段在 shmdt 之后
//    依然存在，所以同一份代码在 Linux 上"看起来是对的"）。
//
// 因此 Windows 下**必须由本进程持有段**（进程退出时随进程释放 → 段自然销毁）。
// 为什么不复用 vendor 的全局句柄：create_shared_memory()/open_shared_memory()
// 是**同一个全局变量**，下一次调用会把它覆盖/关闭；自己再开一个只读句柄，
// 才能保证段在整个进程生命周期内不消失（名字与 vendor 的 SHARED_MEMORY_NAME
// 一致，此处是唯一的硬编码处）。
// ---------------------------------------------------------------------
#ifdef _WIN32
static HANDLE g_keepalive_handle = NULL;
static void keep_segment_alive(void) {
    if (g_keepalive_handle == NULL) {
        g_keepalive_handle = OpenFileMappingA(FILE_MAP_READ, FALSE, "RT_DB_SHARED_MEMORY");
    }
}
#endif

// ---------------------------------------------------------------------
// 创建或打开共享内存，返回映射地址；失败返回 NULL
// *out_created 返回是否是本次新建
// ---------------------------------------------------------------------
static void* create_or_open(size_t size, bool* out_created) {
    *out_created = false;

    int rc = create_shared_memory(size);
    if (rc == 0) {
        *out_created = true;
    } else if (rc == -2) {
        // 已存在 → 打开
        if (open_shared_memory() != 0) {
            fprintf(stderr, "[ems_rt_db] open_shared_memory failed\n");
            return NULL;
        }
    } else {
        fprintf(stderr, "[ems_rt_db] create_shared_memory failed (rc=%d)\n", rc);
        return NULL;
    }

    void* addr = map_shared_memory(0, size);
    if (addr == NULL) {
        fprintf(stderr, "[ems_rt_db] map_shared_memory failed\n");
        destroy_shared_memory(0);
        return NULL;
    }
    return addr;
}

// ---------------------------------------------------------------------
// 判断当前段里是否已经注册过 EMS 点表
// ---------------------------------------------------------------------
static bool has_ems_points(const SharedMemorySegment* seg) {
    return seg != NULL &&
           strncmp(seg->data_points[0].point_id,
                   EMS_POINT_NAMES[0], MAX_POINT_ID_LEN) == 0;
}

// ---------------------------------------------------------------------
// 初始化整个段：清零 → header → 全部 UNUSED_ → 注册 EMS 点表
// ---------------------------------------------------------------------
static void initialize_segment(SharedMemorySegment* seg) {
    memset(seg, 0, sizeof(SharedMemorySegment));

    atomic_init(&seg->header.write_count, 0);
    atomic_init(&seg->header.shutdown_requested, false);
    atomic_init(&seg->header.connected_clients, 0);
#ifdef _WIN32
    seg->header.manager_pid = _getpid();
#else
    seg->header.manager_pid = getpid();
#endif
    seg->header.num_data_points = MAX_DATA_POINTS;
    update_timestamp(&seg->header.last_updated);

    // 先把全部点标记为未使用（build_index_map 会跳过 UNUSED_ 前缀）
    for (size_t i = 0; i < MAX_DATA_POINTS; ++i) {
        snprintf(seg->data_points[i].point_id, MAX_POINT_ID_LEN, "UNUSED_%zu", i);
        seg->data_points[i].point_id[MAX_POINT_ID_LEN - 1] = '\0';
        atomic_init(&seg->data_points[i].value, 0.0);
        atomic_init(&seg->data_points[i].quality, (long)QUALITY_BAD);
        atomic_init(&seg->data_points[i].sequence, 0);
        seg->data_points[i].timestamp = seg->header.last_updated;
        strcpy(seg->data_points[i].units, "");
    }

    // 注册 EMS 的 30 个点（索引 0..29，与枚举顺序一致）
    for (size_t i = 0; i < EMS_POINT_COUNT; ++i) {
        strncpy(seg->data_points[i].point_id, EMS_POINT_NAMES[i], MAX_POINT_ID_LEN - 1);
        seg->data_points[i].point_id[MAX_POINT_ID_LEN - 1] = '\0';
        strncpy(seg->data_points[i].units, EMS_POINT_UNITS[i], MAX_UNIT_LEN - 1);
        seg->data_points[i].units[MAX_UNIT_LEN - 1] = '\0';
        atomic_init(&seg->data_points[i].value, EMS_POINT_DEFAULTS[i]);
        atomic_init(&seg->data_points[i].quality, (long)QUALITY_GOOD);
        atomic_init(&seg->data_points[i].sequence, 0);
        seg->data_points[i].timestamp = seg->header.last_updated;
    }

    atomic_init(&seg->command_queue.head, 0);
    atomic_init(&seg->command_queue.tail, 0);
}

// ---------------------------------------------------------------------
// 对外入口：确保共享内存存在且已注册 EMS 点表
//
//   reset = true  → 强制重建（测试用，保证确定性）
//   reset = false → 已存在且已是 EMS 点表则原样保留（生产用，不丢数据）
//
// 返回 true 表示可用。调用后本函数已释放自己的映射，
// 调用方随后用 rt_db_init() 正常打开即可。
// ---------------------------------------------------------------------
bool ems_rt_db_setup(bool reset, bool* out_created) {
    bool created = false;
    void* addr = create_or_open(sizeof(SharedMemorySegment), &created);
    if (addr == NULL) return false;

    SharedMemorySegment* seg = (SharedMemorySegment*)addr;

    if (created || reset || !has_ems_points(seg)) {
        initialize_segment(seg);
    }

    if (out_created) *out_created = created;

    unmap_shared_memory(addr, sizeof(SharedMemorySegment));

#ifdef _WIN32
    // Windows：**故意不关句柄** —— 段必须活着（见 keep_segment_alive 的说明）。
    // 这里既关掉 vendor 全局句柄，又留下本进程自己的只读存活句柄。
    keep_segment_alive();
    destroy_shared_memory(0);
#else
    destroy_shared_memory(0);
#endif
    return true;
}

// ---------------------------------------------------------------------
// 单独的可执行入口（可选）：
//   ems_rt_db_init.exe [--reset]
// ---------------------------------------------------------------------
#ifdef EMS_RT_DB_STANDALONE
int main(int argc, char** argv) {
    bool reset = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--reset") == 0) reset = true;
    }
    bool created = false;
    if (!ems_rt_db_setup(reset, &created)) {
        fprintf(stderr, "[ems_rt_db] setup FAILED\n");
        return 1;
    }
    printf("[ems_rt_db] ready: %zu points (%s)\n",
           (size_t)EMS_POINT_COUNT, created ? "created" : "opened");
    return 0;
}
#endif
