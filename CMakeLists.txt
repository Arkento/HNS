cmake_minimum_required(VERSION 3.21)

project(NjuLmsCrawler
    VERSION 0.2.0
    DESCRIPTION "智汇南雍 (NJU LMS / TronClass) 作业采集与管理系统"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局语言设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# MSVC 多线程 DLL 运行库，与 Qt 官方预编译包保持一致。
# 必须在任何 target 创建之前设置，否则不会影响已存在的 target。
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# ---------------------------------------------------------------------------
# 构建开关
#
# NJULMS_BUILD_GUI       : 构建 Qt WebEngine 浏览器外壳与主窗口 (阶段 1)
# NJULMS_BUILD_TESTS     : 构建单元测试
# NJULMS_WARNINGS_AS_ERRORS : 把本项目源码的告警当错误
# ---------------------------------------------------------------------------
option(NJULMS_BUILD_GUI "Build the Qt WebEngine GUI shell and MainWindow" ON)
option(NJULMS_BUILD_TESTS "Build unit tests" ON)
option(NJULMS_WARNINGS_AS_ERRORS "Treat warnings in project sources as errors" OFF)

# ---------------------------------------------------------------------------
# Qt
#
# Core / Sql 是所有模块的基础；Gui / Widgets / WebEngineWidgets 仅 GUI 需要。
# 这样在 Qt WebEngine 尚未安装时，核心库与单元测试仍可独立构建与验证。
# ---------------------------------------------------------------------------
set(NJULMS_QT_COMPONENTS Core Sql)

if(NJULMS_BUILD_GUI)
    list(APPEND NJULMS_QT_COMPONENTS Widgets WebEngineWidgets)
endif()

if(NJULMS_BUILD_TESTS)
    list(APPEND NJULMS_QT_COMPONENTS Test)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS ${NJULMS_QT_COMPONENTS})

message(STATUS "NjuLmsCrawler: Qt ${Qt6_VERSION} found (${NJULMS_QT_COMPONENTS})")
message(STATUS "NjuLmsCrawler: GUI=${NJULMS_BUILD_GUI} TESTS=${NJULMS_BUILD_TESTS}")

# ---------------------------------------------------------------------------
# 公共编译选项
# ---------------------------------------------------------------------------
add_library(njulms_build_options INTERFACE)

target_compile_definitions(njulms_build_options INTERFACE
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_DISABLE_DEPRECATED_BEFORE=0x060500
    QT_USE_QSTRINGBUILDER
)

if(MSVC)
    target_compile_options(njulms_build_options INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /MP
    )
    if(NJULMS_WARNINGS_AS_ERRORS)
        target_compile_options(njulms_build_options INTERFACE /WX)
    endif()
else()
    target_compile_options(njulms_build_options INTERFACE -Wall -Wextra -Wpedantic)
    if(NJULMS_WARNINGS_AS_ERRORS)
        target_compile_options(njulms_build_options INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# 子目录
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(NJULMS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
