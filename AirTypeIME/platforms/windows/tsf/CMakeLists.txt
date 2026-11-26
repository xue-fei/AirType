cmake_minimum_required(VERSION 3.20)
project(airtype_tsf LANGUAGES CXX)

# AirType Windows TSF 外壳。
# 第一阶段只构建 M1 风险探针（独立 exe，不注册、不注入）；
# DLL 目标在 TsfTextService 落地后追加。

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "AirType TSF 外壳需要 MSVC（ATL / msctf.h）")
endif()

if(NOT DEFINED CMAKE_SIZEOF_VOID_P OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "TSF 外壳默认构建 x64；32 位进程需要单独的 x86 DLL")
endif()

add_executable(airtype_tsf_probe
    src/TsfProbe.cpp
    src/FfiBridge.cpp
    src/HiddenWindow.cpp
    src/StatusWindow.cpp
    src/ModuleUtil.cpp
    src/TsfGuids.cpp
)

target_include_directories(airtype_tsf_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(airtype_tsf_probe PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(airtype_tsf_probe PRIVATE
    ole32
    oleaut32
    uuid
    UIAutomationCore
    gdiplus
)

# 产物统一输出到 AIRTYPE_TSF_OUT_DIR（默认 <build>/bin），
# 方便 build.ps1 收集，也允许在 DLL 被占用时改输出到旁路目录。
if(NOT DEFINED AIRTYPE_TSF_OUT_DIR)
    set(AIRTYPE_TSF_OUT_DIR "${CMAKE_BINARY_DIR}/bin")
endif()

set_target_properties(airtype_tsf_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${AIRTYPE_TSF_OUT_DIR}"
)

# --- TSF 文本服务 DLL ---
# in-proc COM 服务器，由系统注入到目标进程；不是可执行文件。
add_library(airtype_tsf MODULE
    src/dllmain.cpp
    src/TsfTextService.cpp
    src/TsfEditSession.cpp
    src/HiddenWindow.cpp
    src/StatusWindow.cpp
    src/FfiBridge.cpp
    src/ModuleUtil.cpp
    src/TsfGuids.cpp
    src/airtype_tsf.def
)

target_include_directories(airtype_tsf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(airtype_tsf PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(airtype_tsf PRIVATE
    ole32
    oleaut32
    uuid
    gdiplus
    # UIAutomationCore：UIA 光标回退（Qt/Chromium 等自绘控件定位）需要。
    UIAutomationCore
)

# AIRTYPE_TRACE_STATUS：定位链路追踪日志开关。默认关闭（高频路径），
# 排查「图标不显示 / 跟随异常」时用 -DAIRTYPE_TRACE_STATUS=ON 打开，
# 会打印每拍门禁判定与异步回调结果。
option(AIRTYPE_TRACE_STATUS "打印状态图标定位链路的逐拍日志" OFF)
if(AIRTYPE_TRACE_STATUS)
    target_compile_definitions(airtype_tsf PRIVATE AIRTYPE_TRACE_STATUS)
endif()

set_target_properties(airtype_tsf PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${AIRTYPE_TSF_OUT_DIR}"
    # 去掉 MODULE 库默认的 "lib" 前缀，产物就叫 airtype_tsf.dll
    PREFIX ""
)
