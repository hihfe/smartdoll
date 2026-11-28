cmake_minimum_required(VERSION 3.20)

# C++24 落地：g++-13 的 -std=c++23（要更新语言档时换 clang++-18 / -std=c++2c）
if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER g++-13)
endif()

project(doll LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra)

# 测试：Catch2（系统包；无外网也能装）
find_package(Catch2 3 REQUIRED)

# ===== 底座力学件（vendor/：无外网机器的引入形，见 vendor/README.md） =====
set(DOLL_SYSROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendor/sysroot/usr)

find_library(URING_LIB uring HINTS ${DOLL_SYSROOT}/lib/x86_64-linux-gnu NO_DEFAULT_PATH)

add_library(doll_vendor INTERFACE)
target_include_directories(doll_vendor INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor                    # BS_thread_pool.hpp
    ${DOLL_SYSROOT}/include                               # liburing.h
    ${DOLL_SYSROOT}/include/concurrentqueue               # moodycamel/concurrentqueue.h
)
target_link_libraries(doll_vendor INTERFACE ${URING_LIB})

# 契约头（纯头文件库）：policy.h / media.h / with.h / disp.h / tmr.h
add_library(doll_contracts INTERFACE)
target_include_directories(doll_contracts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(doll_contracts INTERFACE cxx_std_23)

# ===== 模块（M0 基础层） =====
add_library(disp STATIC src/01-dispatcher/disp.cc)
target_link_libraries(disp PUBLIC doll_contracts PRIVATE doll_vendor)
target_include_directories(disp PRIVATE src/01-dispatcher)

add_library(tmr STATIC src/04-timer/tmr.cc)
target_link_libraries(tmr PUBLIC doll_contracts PRIVATE doll_vendor disp)
target_include_directories(tmr PRIVATE src/04-timer)

add_library(ipc STATIC src/02-ipc/ipc.cc)
# disp/tmr 公开传导：ipc.cc 调 01/01 门面（post/submit_op/open_pipe、tmr::after/cancel），
# 静态库消费方（测试/上层）需一并链上
target_link_libraries(ipc PUBLIC doll_contracts disp tmr PRIVATE doll_vendor)
target_include_directories(ipc PRIVATE src/02-ipc)

enable_testing()
add_subdirectory(tests)
