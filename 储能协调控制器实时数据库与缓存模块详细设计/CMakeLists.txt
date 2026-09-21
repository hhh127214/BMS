cmake_minimum_required(VERSION 3.5.1)
project(RT_DB VERSION 1.0.0 LANGUAGES C CXX)

# 设置C标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项
if(MSVC)
    # Windows MSVC 编译器
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    # GCC/Clang 编译器
    add_compile_options(-Wall -Wextra -Wpedantic)
    set(CMAKE_C_FLAGS_DEBUG "-g -O0")
    set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# 包含目录
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/rt_db)

# 查找线程库
find_package(Threads REQUIRED)

# 平台特定的库
if(WIN32)
    # Windows平台不需要额外的共享内存库
    set(PLATFORM_LIBS "")
else()
    # Linux平台需要rt库（用于clock_gettime）
    set(PLATFORM_LIBS rt)
endif()

# 创建实时数据库静态库
add_library(rt_db STATIC
    src/rt_db/rt_db_api.c
)

target_link_libraries(rt_db 
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBS}
)

# 设置库的头文件接口
target_include_directories(rt_db PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# 创建共享内存初始化工具
add_executable(rt_db_init
    src/tools/init_rt_db.c
)

target_link_libraries(rt_db_init
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBS}
)

# 创建测试程序
add_executable(test_rt_db
    test/test_rt_db.c
)

target_link_libraries(test_rt_db
    rt_db
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBS}
)

# 创建容量测试程序
add_executable(capacity_test
    test/capacity_test.c
)

target_link_libraries(capacity_test
    rt_db
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBS}
)

# 安装配置
install(TARGETS rt_db rt_db_init
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES include/rt_db_api.h
    DESTINATION include
)

# 生成pkg-config文件（Linux平台）
if(NOT WIN32)
    configure_file(
        ${CMAKE_SOURCE_DIR}/rt_db.pc.in
        ${CMAKE_BINARY_DIR}/rt_db.pc
        @ONLY
    )
    install(FILES ${CMAKE_BINARY_DIR}/rt_db.pc
        DESTINATION lib/pkgconfig
    )
endif()

# 添加自定义目标用于清理共享内存（仅Linux）
if(NOT WIN32)
    add_custom_target(clean-shm
        COMMAND ipcrm -M `ipcs -m | grep ${USER} | awk '{print $$2}'` 2>/dev/null || true
        COMMENT "Cleaning up shared memory segments"
    )
endif()

# ============================================================
# LVRT Agent — 低压穿越控制器
# ============================================================

# 启用 C++17 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含 LVRT 头文件目录
include_directories(${CMAKE_SOURCE_DIR}/src/lvrt_agent)

# 创建 LVRT Agent 可执行文件
add_executable(lvrt_agent
    src/lvrt_agent/lvrt_controller.cpp
    src/lvrt_agent/main_rtdb.cpp
)

target_link_libraries(lvrt_agent
    rt_db
    ${CMAKE_THREAD_LIBS_INIT}
    ${PLATFORM_LIBS}
)

# 打印配置信息
message(STATUS "RT_DB Configuration Summary:")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C Compiler: ${CMAKE_C_COMPILER}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Install Prefix: ${CMAKE_INSTALL_PREFIX}")

# 编译后运行脚本
if(WIN32)
    message(STATUS "Windows platform detected.")
    message(STATUS "To run the system:")
    message(STATUS "  1. First run: rt_db_init.exe")
    message(STATUS "  2. Then run: test_rt_db.exe")
    message(STATUS "  3. Or run: lvrt_agent.exe (LVRT control agent)")
else()
    message(STATUS "Unix-like platform detected.")
    message(STATUS "To run the system:")
    message(STATUS "  1. First run: ./rt_db_init")
    message(STATUS "  2. Then run: ./test_rt_db")
    message(STATUS "To clean shared memory: make clean-shm")
endif()