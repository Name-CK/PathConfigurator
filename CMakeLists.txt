cmake_minimum_required(VERSION 3.22)

project(PathConfigurator LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PATHCONFIGURATOR_BUILD_TESTS "Build core smoke tests" OFF)
set(PATHCONFIGURATOR_MSVC_INCLUDE "" CACHE PATH "MSVC STL include directory used by host Clang")

# 保持控制台子系统，供 VS Code 任务终端可靠接收进程退出事件。
# 程序启动后会自行隐藏独立控制台，用户仍只看到图形配置窗口。
add_executable(PathConfigurator
    src/main.cpp
    src/core.cpp
    resources/pathconfigurator.rc
)

target_include_directories(PathConfigurator PRIVATE include)
target_compile_definitions(PathConfigurator PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(PathConfigurator PRIVATE /W4 /EHs-c- /GR- /Gy /Gw)
    target_link_options(PathConfigurator PRIVATE /OPT:REF /OPT:ICF)
else()
    target_compile_options(PathConfigurator PRIVATE -Wall -Wextra -Wpedantic -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        target_compile_options(PathConfigurator PRIVATE
            -Oz -flto
            -fno-unwind-tables -fno-asynchronous-unwind-tables
            -fno-stack-protector -fno-threadsafe-statics
        )
        if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
            target_link_options(PathConfigurator PRIVATE -flto -Wl,/OPT:REF,/OPT:ICF)
        else()
            target_link_options(PathConfigurator PRIVATE -flto -Wl,--gc-sections)
        endif()
    endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
    # LLVM 18 需要配套较旧的 MSVC STL；不要误用 VS 18 的 STL。
    if(PATHCONFIGURATOR_MSVC_INCLUDE)
        target_compile_options(PathConfigurator PRIVATE
            -nostdinc++
            "-isystem${PATHCONFIGURATOR_MSVC_INCLUDE}"
            -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
        )
    endif()
    target_link_options(PathConfigurator PRIVATE -fuse-ld=lld)
endif()

target_link_libraries(PathConfigurator PRIVATE user32 gdi32 kernel32 advapi32 comdlg32 shell32 ole32 oleaut32 shlwapi uxtheme comctl32)

if(PATHCONFIGURATOR_BUILD_TESTS)
    enable_testing()
    add_executable(PathConfiguratorCoreTest tests/core_smoke.cpp src/core.cpp)
    target_include_directories(PathConfiguratorCoreTest PRIVATE include)
    target_compile_definitions(PathConfiguratorCoreTest PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND WIN32)
        if(PATHCONFIGURATOR_MSVC_INCLUDE)
            target_compile_options(PathConfiguratorCoreTest PRIVATE
                -nostdinc++
                "-isystem${PATHCONFIGURATOR_MSVC_INCLUDE}"
                -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH
            )
        endif()
        target_link_options(PathConfiguratorCoreTest PRIVATE -fuse-ld=lld)
    endif()
    target_link_libraries(PathConfiguratorCoreTest PRIVATE user32 gdi32 kernel32 advapi32 comdlg32 shell32 ole32 oleaut32 shlwapi)
    add_test(NAME core_smoke COMMAND PathConfiguratorCoreTest)
endif()
