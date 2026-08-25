# STM32 PathConfigurator C++ 重构程序设计书

版本：1.0

状态：已实现；本文档保留设计约束与实现依据。

目标平台：Windows 10/11 x64

源码根目录：仓库根目录

测试方式：`tests/core_smoke.cpp` 在系统临时目录创建独立夹具，不依赖真实 STM32 工程或工具安装。

## 1. 目标与范围

本项目将现有 configure-toolchain.vbs 重构为独立的 Windows 原生 C++ GUI 程序。程序用于配置 STM32 工程开发环境，不负责编译 STM32 固件，也不负责烧录或调试。

程序的工作对象是一个 STM32 工程目录，主要完成：

1. 读取 `.ioc` 的 `ProjectManager.ToolChainLocation` 所指向目录中的 CMakeLists.txt，获得 CMAKE_PROJECT_NAME。
2. 读取工程根目录的 .ioc，获得芯片型号。
3. 读取用户已有的 .vscode/settings.json。
4. 通过原生文件选择器配置 CMake、Ninja、st-arm-clang、GDB、OpenOCD，以及 OpenOCD 的 interface/target 配置文件。
5. 检查选择的路径和 st-arm-clang 配套工具。
6. 检测 STM32CubeCLT（含 Windows 注册表），并自动推导包内工具路径。
7. 根据芯片型号自动匹配 SVD 文件。
8. 更新被 Git 忽略的 settings.json。
9. 更新被 Git 忽略的 CMake 生成目录/CMakeUserPresets.json。
10. 向 VSCode 返回明确的进程退出码。

不在本程序范围内：

- 修改 Windows 系统环境变量。
- 修改 STM32CubeMX 生成的源文件。
- 调用 OpenOCD 烧录固件。
- 启动 Cortex-Debug。
- 自动安装工具链。
- 自动下载工具或 SVD 文件。

## 2. 硬约束

### 2.1 可执行文件体积

Release 版本主程序 .exe 文件必须小于 150KB，验收使用文件实际长度：

~~~powershell
$exe = 'PathConfigurator.exe'
(Get-Item $exe).Length
~~~

150KB 指主程序 PE 文件，不包含：

- .pdb 调试符号。
- Visual C++/LLVM 运行时 DLL。
- Windows 系统 DLL。
- 文档、测试数据和安装程序。

如果要求整个分发目录也小于 150KB，需要重新定义功能范围；原生 GUI、JSON 配置和运行时依赖很难同时满足该指标。

### 2.2 无脚本宿主

最终程序不能依赖：

~~~text
wscript.exe
cscript.exe
mshta.exe
PowerShell
cmd.exe
~~~

GUI、文件选择、提示框和配置写入都由 C++ 程序自身完成。VSCode 直接启动本程序并等待其退出。

### 2.3 配置隔离

机器相关路径只写入：

~~~text
<workspace>\.vscode\settings.json
<workspace>\<ProjectManager.ToolChainLocation>\CMakeUserPresets.json
~~~

这两个文件应加入项目 .gitignore。共享配置只包含 settings.example.json、launch.json、tasks.json 和 CMakePresets.json。

### 2.4 编码

- Win32 API 边界统一使用 UTF-16LE/Wide API。
- JSON 文件内部使用 UTF-8。
- 不使用区域设置相关的窄字符 API。
- 不使用 std::codecvt，显式调用 WideCharToMultiByte 和 MultiByteToWideChar。

## 3. 推荐工具链

### 3.1 IDE 和 SDK

推荐使用 Visual Studio 2022 或 Visual Studio 2022 Build Tools，安装：

- Desktop development with C++。
- Windows 10/11 SDK。
- LLVM/Clang for Windows。
- CMake tools for Windows。

STM32CubeCLT 中的 st-arm-clang 只用于目标固件编译，不能用于编译本 Windows 配置器。

### 3.2 主机编译器

推荐：

~~~text
clang-cl.exe
lld-link.exe
~~~

原因：

- 与 VS2022 的 Windows SDK 和头文件兼容。
- -Oz、LTO 和 LLVM 链接优化对小体积更有利。
- 使用 MSVC ABI，不影响 Win32/COM 调用。
- 不需要引入 MinGW 运行时。

MSVC cl.exe 作为备用方案。若使用 MSVC，必须在 Release 配置中关闭增量链接和调试信息，并使用 /MD；静态 CRT 很容易使程序超过 150KB。

### 3.3 构建工具

使用 CMake 管理项目，使用 Ninja 构建：

~~~text
CMake >= 3.22
Ninja
~~~

建议通过 VS2022 Developer PowerShell 或 Developer Command Prompt 构建，确保 Windows SDK、clang-cl 和 lld-link 已进入环境。

## 4. 体积控制策略

### 4.1 最终选择

~~~text
C++17 语法
原生 Win32 API
COM Common Item Dialog
动态 CRT /MD
clang-cl + lld-link
无异常
无 RTTI
无大型第三方库
~~~

不选用：

- Qt：运行库和部署体积明显超标。
- wxWidgets：同样会引入较大运行时。
- .NET/WinForms/WPF：需要运行时环境。
- MFC：静态链接体积不适合本目标。
- nlohmann/json：模板代码和链接体积不利于 150KB 目标。
- WTL：可以使用，但不会解决 JSON、体积和架构问题。

### 4.2 编译和链接选项

clang-cl 方向的 Release 选项：

~~~text
-Oz
-flto=thin
-fno-exceptions
-fno-rtti
-ffunction-sections
-fdata-sections
-fno-unwind-tables
-fno-asynchronous-unwind-tables
~~~

lld-link 方向的链接选项：

~~~text
/OPT:REF
/OPT:ICF
/INCREMENTAL:NO
/SUBSYSTEM:WINDOWS
/LTCG
~~~

需要根据实际 LLVM 版本验证选项名称，CMake 中不要无条件混用 MSVC 和 clang-cl 专用参数。

### 4.3 代码限制

主程序中禁止使用：

- iostream、fstream、regex、locale。
- std::filesystem，改用 Win32 路径 API。
- C++ 异常。
- RTTI 和 dynamic_cast。
- 大型静态查找表，除非确实需要。
- 全局对象的复杂构造函数。
- system、popen 和 shell 命令拼接。

可以使用少量 STL 基础类型，例如 std::array 和简单的 std::vector，但每次 Release 构建都要重新检查体积。

### 4.4 体积预算

初始预算如下，最终以 Release 构建数据为准：

| 部分 | 预算 |
| --- | ---: |
| PE 头和节区 | 8KB |
| 核心路径/解析逻辑 | 25KB |
| Win32 GUI 和对话框 | 25KB |
| JSONC 增量更新器 | 20KB |
| 资源和字符串 | 8KB |
| 优化余量 | 64KB |
| 合计 | 150KB |

如果第一次实现超过 150KB，优先删除不必要的功能和库，不要先使用 UPX。UPX 可能触发安全软件告警，也会降低工业软件的可维护性和签名兼容性。

## 5. 总体架构

已采用“核心逻辑与 GUI 分离”：

~~~text
命令行入口
    ↓
Win32 GUI（src/main.cpp）
    ↓
核心服务（src/core.cpp）
    ├─ 工程定位与 .ioc 解析
    ├─ CubeCLT / OpenOCD / SVD 检测
    ├─ 配置校验与 JSONC 更新
    └─ 原子文件写入
    ↓
Win32 GUI 或无界面模式
~~~

GUI 只负责显示状态、接收用户选择和提交操作。所有路径检测、项目解析和文件写入均可脱离 GUI 单独运行，方便测试和 VS Code 的 `/validate` 调用。

## 6. 目录和文件设计

当前实现的目录结构：

~~~text
C_Configurator/
├─ .gitignore
├─ CMakeLists.txt
├─ CMakePresets.json
├─ CMakeUserPresets.example.json
├─ README.md
├─ PROGRAM_DESIGN.md
├─ include/
│  └─ core.hpp
├─ src/
│  ├─ core.cpp
│  └─ main.cpp
├─ resources/
│  ├─ pathconfigurator.manifest
│  └─ pathconfigurator.rc
└─ tests/
   └─ core_smoke.cpp
~~~

不复制 STM32 固件源码到 C++ 工具工程。自动测试在系统临时目录创建独立工程和工具链夹具。

## 7. 数据模型

### 7.1 工程上下文

~~~cpp
struct ProjectContext {
    WidePath workspaceRoot;
    WidePath toolChainRoot;
    WidePath cmakeListsPath;
    WidePath iocPath;
    std::string projectName;
    std::string rawDevice;
    std::string device;
};
~~~

rawDevice 保存 .ioc 原始值，device 保存给 Cortex-Debug 使用的规范值。

### 7.2 工具路径

~~~cpp
struct ToolPaths {
    WidePath cmake;
    WidePath ninja;
    WidePath starmClang;
    WidePath gdb;
    WidePath openocd;
    std::wstring openocdInterface;
    std::wstring openocdTarget;
    WidePath svd;
};
~~~

### 7.3 配置选项

~~~cpp
enum class SourceMode {
    ExistingSettings,
    ExampleTemplate
};

struct ConfigureOptions {
    WidePath workspace;
    SourceMode sourceMode = SourceMode::ExistingSettings;
    bool showUi = true;
    bool writePresets = true;
};
~~~

所有核心服务通过结构体返回结果，不使用异常：

~~~cpp
struct Status {
    bool ok;
    uint32_t code;
    std::string message;
};
~~~

## 8. 模块职责

### 8.1 ProjectLocator

输入：工作区路径。

输出：

- `.ioc` 的 `ProjectManager.ToolChainLocation` 所指向目录中的 CMakeLists.txt。
- 工作区第一层的 .ioc 文件。
- .vscode/settings.json。
- .vscode/settings.example.json。
- `.ioc` 的 `ProjectManager.ToolChainLocation` 所指向目录中的 CMakeUserPresets.json。

要求：

- 工作区必须是绝对路径。
- 使用 Win32 API 转换为规范绝对路径。
- 不递归搜索 .ioc，避免误读子项目。
- 找不到由 `ProjectManager.ToolChainLocation` 指定的 CMake 目录时给出明确错误。

### 8.2 ProjectMetadataReader

#### CMake 项目名

支持工程约定的声明：

~~~cmake
set(CMAKE_PROJECT_NAME Project_F407VE)
~~~

实现应忽略空白行、# 注释、双引号和 CRLF/LF 换行。

未找到时返回可区分的状态，不要静默把目录名当作真实 CMake 项目名。GUI 可以提供目录名作为显示备用值，但写入配置前应提示用户。

#### .ioc 芯片名

按优先级读取：

~~~text
Mcu.UserName
ProjectManager.DeviceId
Mcu.CPN
PCC.PartNumber
~~~

支持 UTF-8、UTF-8 BOM 和 UTF-16 文件。解析规则限定为第一层 key=value，不实现完整 INI。

建议保留：

~~~text
STM32F407VET6  → STM32F407VE
STM32F407VETx  → STM32F407VE
~~~

对于 H7 双核、TrustZone 或带核后缀的器件，不应简单截断字符串，应通过映射表或用户选择确定 CM4、CM7 等目标。

### 8.3 CubeCltDetector

从已选工具文件所在目录开始，逐级向上查找：

~~~text
STM32CubeCLT_metadata.bat
~~~

找到后把目录记录为 CubeCLT 根目录。不要执行 metadata 文件，仅检查其存在性。

自动候选路径：

~~~text
CMake\bin\cmake.exe
Ninja\bin\ninja.exe
st-arm-clang\bin\starm-clang.exe
GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe
~~~

OpenOCD 不从 CubeCLT 推导，因为当前项目使用独立的 xPack OpenOCD。

### 8.4 SvdMatcher

首先查找：

~~~text
<CubeCLT>\STMicroelectronics_CMSIS_SVD
~~~

然后查找项目根目录中的后备 SVD。

建议使用候选列表和明确评分：

1. 文件扩展名必须为 .svd。
2. 文件主体必须与芯片系列前缀相同。
3. 匹配长度越长优先级越高。
4. 多核文件必须附加核心选择规则。
5. 找不到时返回空结果，不阻止普通编译配置。

### 8.5 ToolValidator

检查：

- 文件存在且为普通文件。
- 文件名大小写不敏感地符合预期。
- `openocd.exe` 必须存在；scripts 目录仅从其安装位置自动定位，不写入配置文件。
- interface 名只能包含字母、数字、下划线和连字符；target 必须是合法的 `.cfg` 文件名。
- 只要自动定位到 scripts，就必须确认所选 interface/target 的 `.cfg` 文件真实存在。
- starm-clang.exe 所在目录存在配套工具。

配套工具至少包括：

~~~text
starm-clang++.exe
starm-ar.exe
starm-ranlib.exe
starm-nm.exe
starm-objdump.exe
starm-readelf.exe
starm-strip.exe
starm-objcopy.exe
starm-size.exe
~~~

不要启动用户选择的 EXE 来验证路径，避免执行不受信任程序。只使用文件属性、版本资源或目录结构检查。

### 8.6 SettingsStore

读写：

~~~text
<workspace>\.vscode\settings.json
~~~

更新字段：

~~~text
cmake.cmakePath
cmake.useCMakePresets
cmake.generator
cmake.sourceDirectory
cmake.buildDirectory
cmake.configureOnOpen
CMAKE_MAKE_PROGRAM
STARM_CLANG_PATH
CMAKE_TOOLCHAIN_FILE
PATH
CustomCfg.cmakePath
CustomCfg.ninjaPath
CustomCfg.starmClangPath
CustomCfg.gdbPath
CustomCfg.openocdPath
CustomCfg.openocd.interface
CustomCfg.openocd.target
CustomCfg.projectName
CustomCfg.rawDevice
CustomCfg.device
CustomCfg.chipType
CustomCfg.svdFile
~~~

未知字段必须保留，例如：

~~~json
"CustomCfg.111": "D:/custom/path"
~~~

### 8.7 PresetStore

读写：

~~~text
<workspace>\<ProjectManager.ToolChainLocation>\CMakeUserPresets.json
~~~

只更新两个本地预设：

~~~text
Debug-Local
Release-Local
~~~

其他用户预设必须保留。当前 VBS 的整体重写行为不应直接照搬到 C++ 版本。

## 9. 轻量 JSON/JSONC 更新器

为满足体积约束，不使用 nlohmann/json、Boost.JSON 或 Qt JSON。

实现受控的 JsonPatcher：

1. 读取整个 UTF-8 文本。
2. 扫描字符串、对象、数组、数字、布尔值和 null。
3. 正确跳过字符串中的转义引号。
4. 支持 // 和 /* */ 注释，兼容 VSCode JSONC。
5. 记录属性值的起止偏移。
6. 替换已知属性时只替换对应值。
7. 找不到属性时在所属对象的最后一个属性前插入。
8. 写出时保留未知字段、注释和大部分原有格式。

建议使用上下文路径定位属性：

~~~text
根对象 / cmake.configureSettings / CMAKE_MAKE_PROGRAM
根对象 / cmake.environment / PATH
根对象 / CustomCfg.cmakePath
~~~

不能只按键名全局搜索，否则嵌套对象中出现同名键时可能修改错误位置。

JsonPatcher 只支持本程序需要的 JSON 结构，不宣称是通用 JSON 库。解析失败时必须：

- 不覆盖原文件。
- 写入错误日志。
- 显示包含文件路径和偏移位置的错误。

## 10. 原子写入和备份

所有配置写入使用：

~~~text
读取原文件
    ↓
解析并生成新文本
    ↓
写入同目录临时文件 *.tmp
    ↓
FlushFileBuffers
    ↓
MoveFileExW 替换原文件
    ↓
必要时保留 *.bak
~~~

临时文件必须和目标文件位于同一目录，确保替换操作在同一卷内完成。

如果用户选择“从 example”：

1. 读取 settings.example.json。
2. 不直接删除原 settings.json。
3. 生成临时新文件。
4. 原文件成功备份后再替换。

这样即使程序中途崩溃，也不会留下空的 settings.json。

## 11. Win32 GUI 设计

### 11.1 主窗口

主窗口使用固定布局的原生控件：

~~~text
芯片型号（只读）
项目名（只读）
CMake 路径       [浏览]
Ninja 路径       [浏览]
starm-clang 路径 [浏览]
GDB 路径         [浏览]
OpenOCD 路径     [浏览]
调试器 [选择]    目标配置文件 [选择]
SVD 路径         [自动检测/浏览]
状态信息
[重新选择] [从 example 生成] [从 settings 生成] [取消]
~~~

按钮文字必须在 100% 和 150% DPI 下完整显示，不能依赖固定像素假设。

### 11.2 文件选择器

初始化 COM：

~~~cpp
CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
~~~

文件选择使用 IFileOpenDialog：

- FOS_FORCEFILESYSTEM。
- FOS_FILEMUSTEXIST。
- COMDLG_FILTERSPEC 设置过滤提示。

文件夹选择增加 FOS_PICKFOLDERS。文件名在返回后再次校验，不能只依赖过滤器。

### 11.3 提示框

使用 TaskDialogIndirect 显示：

- 工具链检测结果。
- 无效路径列表。
- 从 example 覆盖警告。
- 配置成功结果。

所有错误信息应包含下一步操作，不显示只对开发者有意义的内部异常名称。

## 12. 命令行接口和退出码

程序入口建议为 wWinMain，使用 CommandLineToArgvW 解析参数。

命令行：

~~~text
PathConfigurator.exe /workspace <dir> /mode gui
PathConfigurator.exe /workspace <dir> /mode validate
PathConfigurator.exe /workspace <dir> /mode headless
PathConfigurator.exe /workspace <dir> /template example
PathConfigurator.exe /help
~~~

退出码：

| 代码 | 含义 |
| ---: | --- |
| 0 | 成功、帮助或用户取消 |
| 2 | 命令行参数错误 |
| 3 | 不是有效的 STM32 工程 |
| 4 | 工具路径无效 |
| 5 | JSON/配置文件解析失败 |
| 6 | 文件写入失败 |
| 7 | SVD/芯片信息异常 |

用户主动取消是否与成功都返回 0，需要在文档中固定；VSCode 任务通常只关心非零表示失败。

## 13. VSCode 接入

项目任务应直接调用 EXE：

~~~json
{
    "label": "Configure Local Toolchain",
    "type": "process",
    "command": "${workspaceFolder}/PathConfigurator.exe",
    "args": [
        "/workspace",
        "${workspaceFolder}",
        "/mode",
        "gui"
    ],
    "isBackground": false,
    "problemMatcher": [],
    "presentation": {
        "reveal": "never",
        "close": true
    }
}
~~~

不再使用 wscript.exe、cscript.exe 或 mshta.exe。这样 VSCode 监控的是实际 C++ 进程。程序退出后可以提示用户执行 Developer: Reload Window，不自动调用 VSCode 命令。

## 14. 安全设计

### 14.1 路径安全

- 所有用户选择路径都做规范化。
- 不使用未经验证的路径拼接命令。
- 不将路径传给 cmd.exe。
- 不执行 CMake、GDB、OpenOCD 或用户选择的其他 EXE。
- 写文件前确认目标位于工作区 `.vscode` 或 `.ioc` 指定的 CMake 目录。
- 对符号链接和重解析点做明确策略，默认只允许目标目录内写入。

### 14.2 文件安全

- 写入采用临时文件和原子替换。
- 发现解析失败时不覆盖原文件。
- 可选生成 .bak，最多保留一个最近备份。
- 日志不记录工具输出内容，只记录路径检测和错误代码。

### 14.3 GUI 安全

- 不加载网络资源。
- HTA、脚本和浏览器控件不再参与执行。
- 窗口只创建本地 Win32 控件。
- 使用清单启用公共控件和 DPI 支持。

## 15. 日志和诊断

默认不创建日志文件，避免污染项目目录。

发生错误时写入：

~~~text
%LOCALAPPDATA%\PathConfigurator\PathConfigurator.log
~~~

日志字段：

~~~text
时间
版本
工作区路径
操作阶段
错误码
错误消息
~~~

不记录密码、令牌或完整环境变量。工具路径可以记录，但应提供关闭日志选项。

## 16. CMake 设计

主程序使用 WIN32 子系统：

~~~cmake
cmake_minimum_required(VERSION 3.22)
project(PathConfigurator LANGUAGES CXX)

add_executable(PathConfigurator WIN32
    src/main.cpp
    src/core/...
    src/platform/win32/...
    src/ui/win32/...
    resources/PathConfigurator.rc
)

target_compile_features(PathConfigurator PRIVATE cxx_std_17)
~~~

Release 配置必须包含：

~~~text
关闭异常
关闭 RTTI
关闭增量链接
启用链接时优化
启用死代码消除
使用动态 CRT
~~~

建议建立两个 CMake Preset：

~~~text
Debug-Host
Release-MinSize
~~~

Release-MinSize 才是 150KB 验收配置，不能使用 Debug 输出作为体积依据。

## 17. 单元测试设计

测试覆盖：

- 从 `.ioc` 指定目录的 CMakeLists.txt 提取工程名。
- 从 `.ioc` 提取 STM32F407 芯片型号。
- 检测 CubeCLT metadata 文件。
- 匹配 STM32F407.svd。
- 检查 CMake、Ninja、starm-clang、GDB 和 OpenOCD。
- 保留 settings.json 未知字段。
- 保留 CMakeUserPresets.json 未管理的预设。
- JSONC 注释不导致解析失败。
- JSON 解析失败时原文件不变。
- 写入失败时临时文件可清理。
- /help、/validate 和 /mode gui 返回正确退出码。

测试不修改真实 CubeMX 工程。测试在系统临时目录创建独立工程夹具，结束后删除该临时目录。

## 18. 体积验收流程

每次 Release 构建执行：

~~~powershell
cmake --preset Release-MinSize
cmake --build --preset Release-MinSize
~~~

然后检查：

~~~powershell
$exe = 'build/Release-MinSize/PathConfigurator.exe'
$size = (Get-Item $exe).Length
if ($size -ge 153600) { throw "EXE exceeds 150KB: $size bytes" }
~~~

同时使用 llvm-readobj 或 dumpbin 检查是否意外引入：

- 静态 CRT。
- 调试节。
- 增量链接节。
- 大型资源。
- 未使用的第三方库。

体积验收必须与功能测试同时通过。不能为了体积删除必要的错误处理、原子写入或路径安全检查。

## 19. 分阶段实施计划

### 阶段 0：工程初始化

程序员/A 完成：

1. 创建 C_Configurator CMake 工程。
2. 添加 Debug-Host 和 Release-MinSize Preset。
3. 验证 VS2022、clang-cl、Windows SDK 和 Ninja。
4. 实现只显示帮助文字的空程序。
5. 记录初始 EXE 体积。

验收：空程序可以从 VSCode 任务启动并正常退出。

### 阶段 1：核心路径和元数据

实现：

- ProjectLocator。
- ProjectMetadataReader。
- UTF-8/UTF-16 文件读取。
- CMake 项目名解析。
- .ioc 芯片解析。

验收：命令行 /validate 可以输出项目名和芯片型号。

### 阶段 2：工具检测和 SVD

实现：

- CubeCltDetector。
- ToolValidator。
- SvdMatcher。
- OpenOCD scripts 候选搜索，并从实际 `target/*.cfg` 中匹配芯片型号。

验收：针对临时 STM32F407 夹具得到正确 CubeCLT、工具、OpenOCD target 和 STM32F407.svd。

### 阶段 3：轻量 JSONC 更新器

实现：

- JSONC 扫描器。
- 已知字段定位。
- 未知字段保留。
- 原子写入。
- example/settings 两种模式。

验收：配置文件解析、更新、失败恢复测试全部通过。

### 阶段 4：Win32 GUI

实现：

- 主窗口。
- 原生文件选择器。
- 文件夹选择器。
- TaskDialog 提示框。
- DPI 和中文字体适配。

验收：无 HTA、无脚本、无临时 GUI 文件，正常完成一次配置。

### 阶段 5：VSCode 集成

实现：

- tasks.json 直接调用 EXE。
- /workspace 参数。
- isBackground: false。
- 明确退出码。

验收：VSCode 能准确判断任务结束；修改路径后可重新加载并编译、烧录、调试。

### 阶段 6：体积优化和发布

完成：

- 移除不必要的 STL 和库。
- 开启 LTO、死代码消除和最小体积优化。
- 生成版本信息。
- 运行单元测试和体积检查。
- 在企业安全软件环境验证。

验收：Release EXE 小于 150KB，功能和安全测试通过。

## 20. 主要风险和取舍

### 风险一：150KB 与 C++ GUI 的冲突

C++ 标准库、静态 CRT、异常和大型 JSON 库都可能使程序超过 150KB。必须把 150KB 解释为主 EXE 体积，并使用动态 CRT。若要求完全单文件、无运行时 DLL，应准备放宽体积指标或改用纯 C 和更小的对话框实现。

### 风险二：JSONC 解析复杂度

为了保留未知字段和注释，轻量更新器仍需要处理字符串、转义、嵌套对象和注释。第一版本可以限定支持当前项目结构，但必须在解析失败时拒绝覆盖文件。

### 风险三：芯片名称不统一

不同 CubeMX 版本和器件系列的 .ioc 字段可能不同。必须保留原始值，并为双核、TrustZone 等复杂芯片提供显式映射或 GUI 选择。

### 风险四：WTL 维护成本

WTL 可以减少窗口消息代码，但不会自动解决配置模型、JSON 和体积问题。如果团队没有 WTL 经验，推荐坚持原生 Win32；如果已有 WTL 基础，可以只替换 ui/win32 层。

## 21. 最终推荐结论

最终落地方案：

~~~text
C++17
Visual Studio 2022 工具环境
clang-cl + lld-link
CMake + Ninja
原生 Win32 API + COM Common Item Dialog
自研轻量 JSONC 增量更新器
动态 CRT
关闭异常和 RTTI
不使用 WTL、Qt、MFC 或大型 JSON 库
~~~

开发顺序必须是“先核心、后 GUI、最后体积优化”。不要一开始复制 VBS 的所有界面逻辑到 C++ 窗口中；先让 /validate、项目解析、工具检测和安全写入稳定，再增加 GUI。这样程序员/A 可以在每个阶段独立验收，也更容易定位 150KB 体积超标的来源。

## 22. CMake 目标配置

### 22.1 目录与接入

目标配置不假定 CMake 目录名为 `ToolChain`。程序始终使用 `.ioc` 的
`ProjectManager.ToolChainLocation`：该值为相对目录时解析到工程根目录下，
该值为空时 CMake 目录就是工程根目录。

~~~text
<.ioc 指定的 CMake 目录>/
├─ CMakeLists.txt
├─ project-config.json
└─ cmake/
   └─ PathConfiguratorProject.cmake
~~~

首次点击“保存 CMake 目标配置”时，程序在 `CMakeLists.txt` 的用户目标配置
之前插入一次：

~~~cmake
include("${CMAKE_CURRENT_LIST_DIR}/cmake/PathConfiguratorProject.cmake")
~~~

`project-config.json` 和生成的 `.cmake` 都是工程规则，应提交 Git；本机
工具链路径仍只写入 `.vscode/settings.json` 与 `CMakeUserPresets.json`。

### 22.2 数据格式

~~~json
{
  "version": 1,
  "sources": [
    { "kind": "file", "path": "../App/Src/app_main.c" },
    { "kind": "folder", "path": "../BSP/Src" }
  ],
  "includeDirectories": ["../App/Inc", "../BSP/Inc"],
  "compileDefinitions": ["USE_BSP", "APP_VERSION=1"],
  "linkDirectories": ["../Middlewares/Library"]
}
~~~

全部路径相对于 `.ioc` 指定的 CMake 目录，不保存用户机器上的绝对路径。
路径、宏中禁止换行、引号、分号和 CMake 变量展开符，避免把可视化配置变成
任意 CMake 代码注入入口。

### 22.3 目录源文件规则

`kind: "folder"` 会生成：

~~~cmake
file(GLOB_RECURSE PathConfiguratorProject_FOLDER_0 CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/../../BSP/Src/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/../../BSP/Src/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../../BSP/Src/*.s"
)
~~~

它会递归收集 C、C++ 和汇编源文件，包含子目录。头文件不需要加入
`target_sources`，应通过“头文件目录”配置；其它任意文件也不会被误作为
编译单元。`CONFIGURE_DEPENDS` 会让 CMake 在后续构建检查中发现新增或删除的源文件，
但使用者在首次保存后仍应在 CMake Tools 执行一次 Configure。
