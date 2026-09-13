// =====================================================================
// EMS 点表初始化器 —— 对外声明（C / C++ 共用）
//
//   reset = true  → 强制重建共享内存与点表（测试用，保证确定性）
//   reset = false → 已存在且已是 EMS 点表则保留（生产用，不丢数据）
//
// 返回 true 表示共享内存已就绪。调用后本函数已释放自己的映射，
// 调用方随后用 rt_db_init() 正常打开即可。
// =====================================================================

#ifndef EMS_RT_DB_SETUP_H
#define EMS_RT_DB_SETUP_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool ems_rt_db_setup(bool reset, bool* out_created);

#ifdef __cplusplus
}
#endif

#endif // EMS_RT_DB_SETUP_H
