# STM32 PathConfigurator

`STM32 PathConfigurator` 是一个 Windows 原生图形配置工具。它让刚接触 STM32 的使用者可以用 STM32CubeMX 生成代码，再直接在 VS Code 中完成 **编译、烧录和单步调试**；无需设置系统环境变量，也不依赖 STM32 for VSCode 插件。

它不会安装或修改任何工具，而是把已安装工具的本机路径写入工程的本地配置文件。

## 从零快速开始

下面的步骤适用于第一次使用 VS Code 开发 STM32 的使用者。完成一次后，后续工程只需复制模板并运行配置器。

### 1. 准备软件和调试器

| 软件或硬件 | 是否需要 | 作用与下载地址 |
| --- | --- | --- |
| STM32CubeMX | 需要 | 创建 `.ioc` 并生成 CMake 工程：[STM32CubeMX 官方页](https://www.st.com/en/development-tools/stm32cubemx.html) |
| STM32CubeCLT | 需要 | 提供 CMake、Ninja、`starm-clang` 和 ARM GDB：[STM32CubeCLT 官方页](https://www.st.com/en/development-tools/stm32cubeclt.html) |
| OpenOCD | 需要 | 与 CMSIS-DAP、ST-LINK 等调试器通信；推荐 Windows 预编译包：[xPack OpenOCD 下载页](https://xpack.github.io/dev-tools/openocd/) |
| Visual Studio Code | 需要 | 编辑、任务执行和调试界面：[VS Code 下载页](https://code.visualstudio.com/Download) |
| C/C++ 扩展 | 建议 | 头文件跳转、代码补全与错误提示：[C/C++](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) |
| Cortex-Debug 扩展 | 调试需要 | 在 VS Code 中通过 OpenOCD 调试 STM32：[Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug) |
| CMake Tools 扩展 | 建议 | 提供 CMake 预设、配置和构建的状态栏入口：[CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) |
| vscode-tasks 扩展 | 可选 | 为 `tasks.json` 中的任务提供快捷运行入口：[actboy168/vscode-tasks](https://github.com/actboy168/vscode-tasks) |
| CMSIS-DAP、ST-LINK 或其他 OpenOCD 支持的调试器 | 烧录/调试需要 | 将调试器以 SWD 连接到目标板；CMSIS-DAP 是模板的默认选择。 |

`tasks.json` 由 VS Code 内置支持，**不安装 vscode-tasks 也能运行任务**。安装它只是为了更方便地点击任务。STM32CubeCLT 已包含本流程所需的 CMake、Ninja、编译器和 GDB，一般不需要另行安装这些工具。

### 2. 用 CubeMX 生成 CMake 工程

在 STM32CubeMX 中新建或打开工程，然后进入 **Project Manager**：

1. 将 `Toolchain / IDE` 选择为 `CMake`。
2. `ToolChainLocation` 可填写例如 `ToolChain`，也可以留空；留空时 CMake 文件会在工程根目录。
3. 点击 **Generate Code**。

生成后，工程根目录中必须有 `.ioc` 文件，并且 `.ioc` 内应包含 `ProjectManager.TargetToolchain=CMake`。配置器会自动按 `ToolChainLocation` 查找 `CMakeLists.txt`，不要求目录名称固定为 `ToolChain`。

### 3. 复制 VS Code 模板和配置器

本仓库提供可直接复制的模板：[templates/.vscode](templates/.vscode)。将其中**全部文件**复制到 CubeMX 工程根目录下的 `.vscode`，再将发布包中的 `PathConfigurator.exe` 也放进该目录：

```text
MyStm32Project/
├─ MyStm32Project.ioc
├─ ToolChain/                         # 由 .ioc 的 ToolChainLocation 决定，名称不固定
│  └─ CMakeLists.txt
└─ .vscode/
   ├─ PathConfigurator.exe            # 从本仓库 Releases 下载
   ├─ settings.example.json            # 模板，提交 Git
   ├─ tasks.json                       # 编译、烧录和配置任务，提交 Git
   └─ launch.json                      # Cortex-Debug 配置，提交 Git
```

发布版 EXE 从本仓库的 [Releases](../../releases) 页面获取。不要先创建 `settings.json`；它由配置器为当前电脑生成。

将以下内容加入 **STM32 工程自身** 的 `.gitignore`：

```gitignore
.vscode/PathConfigurator.exe
.vscode/settings.json
.vscode/settings.json.bak
**/CMakeUserPresets.json
**/CMakeUserPresets.json.bak
```

### 4. 运行一次本机工具链配置

1. 在 VS Code 中选择 **File: Open Folder**，打开**工程根目录**，不要只打开 `ToolChain` 目录。
2. 按 `Ctrl+Shift+P`，执行 **Tasks: Run Task**，选择 `Configure Local Toolchain (GUI)`。
3. 在弹出的窗口中选择本机的 `cmake.exe`、`ninja.exe`、`starm-clang.exe`、`arm-none-eabi-gdb.exe` 和 `openocd.exe`。
4. 使用 CMSIS-DAP 时选择 `cmsis-dap`；使用 ST-LINK 时选择 `stlink`。程序会在 OpenOCD 的 `interface`、`target` 目录中校验文件，并为当前芯片尝试匹配 target 和 SVD。
5. 首次使用点击 `确认并生成（从 example）`。程序会使用模板创建本机 `.vscode/settings.json`；已有本机 `settings.json` 时，选择 `确认并生成（从 settings）` 可保留其他个人设置。
6. 生成后执行 VS Code 命令 **Developer: Reload Window**。

选择 CMake、Ninja 或 `starm-clang` 后，程序会向上检查 `STM32CubeCLT_metadata.bat`。如果识别到 STM32CubeCLT，会提示一次性补齐包内的 CMake、Ninja、starm-clang、GDB 路径，并尝试匹配当前芯片的 SVD。选择 `openocd.exe` 后，程序自动寻找其 `scripts` 目录，不需要单独配置。

### 5. 编译、烧录和调试

配置完成后，继续使用 **Tasks: Run Task**：

1. `Build Debug`：生成 `Debug` 版本 ELF、HEX、BIN 等文件。
2. `Flash Debug (OpenOCD)`：自动先编译，再通过已选择的 OpenOCD 调试器烧录、校验并复位。
3. 按 `F5`，选择 `Debug With OpenOCD`：Cortex-Debug 会自动先编译，然后下载程序并停在 `main` 附近，可设置断点和查看 SVD 外设寄存器。

需要优化和较小体积时执行 `Build Release`。`CMake: Clean Debug` 会删除整个 Debug 构建目录，用于解决移动工程后 CMakeCache 路径不一致等问题。

## 它还解决多人协作问题

多人共用 STM32 工程时，代码仓库中的 CMake、任务和调试配置可以保持一致，但每个人安装工具的位置通常不同，例如：

```text
D:\Tool\ST\STM32CubeCLT_1.22.0
C:\ST\STM32CubeCLT_1.22.0
E:\Tools\OpenOCD
```

本程序将这些本机路径保存到 `.vscode/settings.json` 和 `CMakeUserPresets.json`。这两个文件不应提交 Git；工程共用的 `tasks.json`、`launch.json`、`settings.example.json`、`project-config.json` 和 CMake 文件仍可正常提交。

程序标题栏会显示当前版本。通过菜单 **帮助 -> 检查更新** 可查询 GitHub Releases 的最新稳定版本；发现新版本后可以直接打开下载页面。检查更新只读取 GitHub 的公开 Release 信息，不会上传工程文件或本机配置。

对于 STM32H7，CubeMX/CMSIS 会把 st-arm-clang（Clang）识别为 GCC，并对一个 Clang 不支持的 CMSIS 属性给出兼容警告。配置器会在工程的 `cmake/PathConfiguratorCompilerCompat.cmake` 中仅对 Clang 关闭该警告，并在 CubeMX 源码子目录之前接入它。这不修改任何 CubeMX/CMSIS 头文件；该模块应提交 Git，后续重新生成代码不需要再次运行配置器。

## 工具路径与配置方式

也可以双击 `.vscode/PathConfigurator.exe`，或在终端指定工程目录：

```powershell
.\.vscode\PathConfigurator.exe /workspace "D:\Code\MyStm32Project"
```

程序会从指定目录向上查找 `.ioc` 和由 CubeMX 生成的 CMake 代码。若 `.ioc` 不是 CMake 项目，或尚未生成代码，会显示原因并退出。

窗口中各路径的含义如下：

| 配置项 | 选择内容 |
| --- | --- |
| CMake | `cmake.exe` |
| Ninja | `ninja.exe` |
| starm-clang | `starm-clang.exe` |
| ARM GDB | `arm-none-eabi-gdb.exe` |
| OpenOCD | `openocd.exe` |
| 调试器 | 常用 `cmsis-dap.cfg` 或 `stlink.cfg`；也可点“选择...”选择其它 interface 配置 |
| 目标配置文件 | 例如 `stm32f4x.cfg`、`stm32h7x.cfg`；可手动编辑或从 OpenOCD 的 `target` 目录选择 |
| SVD | 芯片对应的 `.svd` 文件 |

当工程已经有 `.vscode/settings.json` 时，使用 `确认并生成（从 settings）` 更新配置器负责的键，并保留其他个人 VS Code 设置；`确认并生成（从 example）` 则从模板重新生成配置。

## 默认配置和自动检测

每次读取路径都会检查文件是否存在、文件名是否正确，以及 OpenOCD 配置是否有效。启动时按下列优先级处理：

1. 工程 `.vscode/settings.json` 中有效的路径。
2. `%LOCALAPPDATA%\PathConfigurator\user-settings.json` 中的默认路径，程序会先询问是否使用。
3. Windows 注册表中检测到的 STM32CubeCLT，程序会先询问是否补充。
4. 用户在界面中手动选择的路径。

默认配置只保存跨工程通用的工具路径。读取默认配置后，若当前工程尚未配置 SVD，程序会根据 `.ioc` 中的芯片型号和已识别的 STM32CubeCLT 自动匹配 SVD；OpenOCD target 同样会按当前芯片自动选择，仍可在界面中手动修改。

“配置”菜单提供：

- `保存为默认配置`：保存 CMake、Ninja、starm-clang、GDB、OpenOCD 和调试器 interface，供其他 STM32 工程复用。
- `读取默认配置`：读取并覆盖界面中的共享工具路径。
- `从注册表检查 CLT 包`：手动搜索 STM32CubeCLT 安装记录。

默认配置**不会**保存 SVD、OpenOCD target、芯片类型或项目名，因为它们属于具体工程。

## VS Code 编译、烧录和调试

本程序只写入路径和相关 VS Code 配置键；它不会直接编译、烧录或启动调试器。工程的 `.vscode/tasks.json` 和 `.vscode/launch.json` 需要使用以下 `CustomCfg.*` 键：

| 键 | 用途 |
| --- | --- |
| `CustomCfg.cmakePath` | CMake 可执行文件 |
| `CustomCfg.ninjaPath` | Ninja 可执行文件 |
| `CustomCfg.starmClangPath` | STM32 st-arm-clang 编译器 |
| `CustomCfg.gdbPath` | Cortex-Debug 使用的 GDB |
| `CustomCfg.openocdPath` | OpenOCD 可执行文件 |
| `CustomCfg.openocd.interface` | OpenOCD interface 文件名，不含 `.cfg` |
| `CustomCfg.openocd.target` | OpenOCD target 文件名，含 `.cfg` |
| `CustomCfg.svdFile` | Cortex-Debug 使用的 SVD 文件 |
| `CustomCfg.projectName` | CubeMX CMake 项目名 |
| `CustomCfg.toolchainLocation` | `.ioc` 指定的 CMake 目录，相对工程根目录 |

Cortex-Debug 的关键配置示例：

```jsonc
{
  "type": "cortex-debug",
  "request": "launch",
  "servertype": "openocd",
  "serverpath": "${config:CustomCfg.openocdPath}",
  "gdbPath": "${config:CustomCfg.gdbPath}",
  "configFiles": [
    "interface/${config:CustomCfg.openocd.interface}.cfg",
    "target/${config:CustomCfg.openocd.target}"
  ],
  "svdFile": "${config:CustomCfg.svdFile}"
}
```

完成配置后，典型工作流为：

1. 执行 `CMake: Configure Debug`。
2. 执行 `Build Debug`。
3. 执行 `Flash Debug (OpenOCD)`，或按 `F5` 交给 Cortex-Debug 烧录并调试。

任务名称由工程自己的 `tasks.json` 决定；上述名称只是推荐约定。

## CMake 目标配置页

“CMake 目标配置”页用于把用户代码纳入 CubeMX 的 CMake 工程，可维护：

- 目标文件 / 文件夹
- 头文件目录
- 编译宏
- 链接目录

路径统一以工程根目录显示，例如 `\App`、`\BSP`。双击已有行可直接编辑；双击最后的空白行可新增。也可使用“添加文件...”或“添加文件夹...”。

选择源文件夹后，生成的 CMake 使用 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 递归收集 C、C++ 和汇编源文件。保存时，程序会在 `.ioc` 所指定的 CMake 目录中生成并更新：

```text
project-config.json
cmake/PathConfiguratorProject.cmake
```

首次保存还会在同一目录的 `CMakeLists.txt` 中加入一次：

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/cmake/PathConfiguratorProject.cmake")
```

这三个文件属于工程构建规则，应提交 Git。新增或删除递归目录内的源文件后，重新执行 CMake Configure。

首次确认工具链配置还会生成 `cmake/PathConfiguratorCompilerCompat.cmake`，并在同一目录的 `CMakeLists.txt` 中于 `add_executable(...)` 与 `add_subdirectory(cmake/stm32cubemx)` 之前加入一次 `include(...)`。它仅在 st-arm-clang/Clang 下抑制 CubeMX H7 CMSIS 的 `optimize("Os")` 兼容警告，不改动 CMSIS 头文件；这个文件同样应提交 Git。

## 程序读写范围

| 类型 | 文件或位置 |
| --- | --- |
| 读取 | 工程 `.ioc`、CMakeLists.txt、`.vscode/settings.json`、`.vscode/settings.example.json`、用户默认配置、STM32CubeCLT 注册表项 |
| 写入 | `.vscode/settings.json`、`CMakeUserPresets.json`、`project-config.json`、`cmake/PathConfiguratorProject.cmake`、`cmake/PathConfiguratorCompilerCompat.cmake` |
| 首次保存目标配置时修改 | CMakeLists.txt：只补入一次 `include(...)` |
| 不修改 | 系统环境变量、`.ioc`、CubeMX 生成的源代码、OpenOCD 配置文件、固件内容 |

写入前程序会创建临时文件；写入成功后替换原文件并保留 `.bak` 备份。

## 命令行参数

```text
PathConfigurator.exe
PathConfigurator.exe /workspace <STM32工程目录>
PathConfigurator.exe /validate <STM32工程目录>
PathConfigurator.exe /help
```

发布新版本时，请同步修改 `src/main.cpp` 中的 `kAppVersion`，创建对应的 Git 标签（例如 `v1.0.1`），并将构建产物上传到 GitHub Release。

`/validate` 只验证工程 `.vscode/settings.json`，不会修改文件。GUI 正常关闭和配置完成均会返回退出码 `0`；配置错误、工程不符合条件或验证失败会返回非零退出码，便于 VS Code 任务判断结果。

## 常见问题

**提示不是 CMake 工程或未生成 CMake 代码**

在 STM32CubeMX 的 Project Manager 中选择 `Toolchain / IDE = CMake`，设置或确认 `ToolChainLocation`，然后重新生成代码。

**CMake 提示 `Set STARM_CLANG_PATH`**

重新运行配置器，选择有效的 `starm-clang.exe`，再点击“确认并生成”。生成后的 `CMakeUserPresets.json` 是每位开发者本机文件，不应从其他电脑复制。

**Cortex-Debug 找不到 OpenOCD 或 GDB**

确认 `launch.json` 使用的是上文的 `CustomCfg.openocdPath` 和 `CustomCfg.gdbPath`，然后在配置器中选择对应的实际 `.exe` 文件。

**切换芯片后 target 或 SVD 不匹配**

重新运行配置器。选择或自动识别 STM32CubeCLT 后，程序会按 `.ioc` 中的芯片型号匹配 SVD；OpenOCD target 仍可在界面中手动选择。
