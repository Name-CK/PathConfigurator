# STM32 PathConfigurator

`STM32 PathConfigurator` 是一个 Windows 图形工具，用于将 STM32CubeMX 生成的 **CMake 工程**接入 VS Code。完成一次本机配置后，可以直接在 VS Code 中：

- 编译 Debug 或 Release 固件
- 通过 OpenOCD 烧录开发板
- 使用 Cortex-Debug 设置断点、单步执行和查看外设寄存器

它不要求设置系统环境变量，也不依赖 STM32 for VSCode 插件。工具安装在每个人电脑上的位置只保存在本机文件中，因此适合新手入门，也适合多人共用同一 STM32 工程。

> 本文是使用手册，不介绍程序源代码的组织方式或内部实现。

## 1. 快速入手

本章面向第一次接触 STM32、VS Code 和 CMake 的使用者。只需按顺序完成即可，不必先理解这些名词。

### 1.1 安装软件并记录位置

先安装下表中的软件。安装完成后，记住或在资源管理器中找到表中列出的 `.exe` 文件；稍后配置器会让你逐一选择它们。

| 软件 | 是否需要 | 下载地址 | 安装后需要找到的内容 |
| --- | --- | --- | --- |
| STM32CubeMX | 必需 | [官方页面](https://www.st.com/en/development-tools/stm32cubemx.html) | `STM32CubeMX.exe`。它负责创建 STM32 工程和生成基础代码。 |
| STM32CubeCLT | 必需 | [官方页面](https://www.st.com/en/development-tools/stm32cubeclt.html) | `CMake\bin\cmake.exe`、`Ninja\bin\ninja.exe`、`st-arm-clang\bin\starm-clang.exe`、`GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe`。 |
| OpenOCD | 烧录和调试必需 | [xPack OpenOCD](https://xpack.github.io/dev-tools/openocd/) | `openocd.exe`。建议使用解压后的 Windows 预编译包。 |
| Visual Studio Code | 必需 | [下载页面](https://code.visualstudio.com/Download) | 不需要记录路径。它是编写、编译和调试代码的编辑器。 |
| C/C++ 扩展 | 建议 | [C/C++](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) | 提供代码补全、错误提示和跳转到头文件。 |
| CMake Tools 扩展 | 建议 | [CMake Tools](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cmake-tools) | 提供 CMake 的配置、构建和启动入口。 |
| Cortex-Debug 扩展 | 调试必需 | [Cortex-Debug](https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug) | 让 VS Code 可以通过 OpenOCD 调试 STM32。 |
| vscode-tasks 扩展 | 可选 | [actboy168/vscode-tasks](https://github.com/actboy168/vscode-tasks) | 将常用任务显示在 VS Code 状态栏；不安装也能从命令面板运行任务。 |

还需要一个已经连接到目标板的调试器，例如 CMSIS-DAP 或 ST-LINK。它负责把电脑与 STM32 芯片连接起来。确认开发板已供电，并正确连接 SWDIO、SWCLK、GND；必要时还应连接 NRST。

以 STM32CubeCLT 安装到 `D:\Tool\ST\STM32CubeCLT_1.22.0` 为例，后面需要选择的文件通常是：

```text
D:\Tool\ST\STM32CubeCLT_1.22.0\CMake\bin\cmake.exe
D:\Tool\ST\STM32CubeCLT_1.22.0\Ninja\bin\ninja.exe
D:\Tool\ST\STM32CubeCLT_1.22.0\st-arm-clang\bin\starm-clang.exe
D:\Tool\ST\STM32CubeCLT_1.22.0\GNU-tools-for-STM32\bin\arm-none-eabi-gdb.exe
D:\Tool\xpack-openocd-0.12.0-7\bin\openocd.exe
```

STM32CubeCLT 已包含本流程所需的 CMake、Ninja、编译器和 GDB，不要再为这些组件单独安装其他版本。

### 1.2 创建并配置第一个项目

#### 第一步：用 STM32CubeMX 生成代码

1. 打开 STM32CubeMX，选择芯片或开发板并完成引脚、时钟和外设设置。
2. 打开 **Project Manager** 页面。
3. 将 `Toolchain / IDE` 设为 `CMake`。
4. `ToolChainLocation` 可以填 `ToolChain`，也可以留空。它只决定 CMake 文件放在哪里，不影响配置器使用。
5. 点击 **Generate Code**。

生成成功后，工程根目录应包含一个 `.ioc` 文件。例如：

```text
MyStm32Project/
├─ MyStm32Project.ioc
├─ Core/
├─ Drivers/
└─ ToolChain/                 # 此目录名称可能不同，也可能不存在
   └─ CMakeLists.txt
```

#### 第二步：复制 VS Code 模板

从本仓库的 [templates/.vscode](templates/.vscode) 复制整个 `.vscode` 文件夹到 STM32 工程根目录。若获取的是源码仓库而不是发布包，还需要从 [Releases](../../releases) 下载 `PathConfigurator.exe`，放入这个 `.vscode` 文件夹。

最终目录应类似：

```text
MyStm32Project/
├─ MyStm32Project.ioc
├─ ToolChain/
│  └─ CMakeLists.txt
└─ .vscode/
   ├─ PathConfigurator.exe
   ├─ extensions.json
   ├─ launch.json
   ├─ settings.example.json
   └─ tasks.json
```

打开 VS Code，选择 **File > Open Folder**，并选择最外层的 `MyStm32Project` 文件夹。不要只打开 `ToolChain` 或 `Core` 文件夹。

VS Code 若提示安装推荐扩展，选择安装；否则按 `Ctrl+Shift+X` 搜索并安装上一节列出的扩展。

#### 第三步：运行配置器

1. 按 `Ctrl+Shift+P`，输入并执行 **Tasks: Run Task**。
2. 选择 `Configure Local Toolchain (GUI)`。
3. 在弹出的窗口中选择 1.1 节记录的 `cmake.exe`、`ninja.exe`、`starm-clang.exe`、`arm-none-eabi-gdb.exe` 和 `openocd.exe`。
4. 使用 CMSIS-DAP 时，调试接口选择 `cmsis-dap.cfg`；使用 ST-LINK 时选择 `stlink.cfg`。目标芯片配置会按 `.ioc` 中的芯片型号自动建议，仍可手动修改。
5. SVD 通常会自动找到。它用于调试时显示寄存器；只有自动匹配失败时才需要手动选择 `.svd` 文件。
6. 第一次配置请点击 **重建**。它会根据 `settings.example.json` 创建当前电脑专用的 `settings.json` 和 `CMakeUserPresets.json`。
7. 返回 VS Code 后执行 **Developer: Reload Window**，使扩展读取新配置。

选择 CMake、Ninja、starm-clang 或 GDB 后，配置器会自动检查它们是否属于同一个 STM32CubeCLT 安装包，并可一次性补齐其它路径。选择 `openocd.exe` 后，程序会自动查找 OpenOCD 的 `scripts` 目录，无需额外填写。

#### 第四步：编译、烧录和调试

再次按 `Ctrl+Shift+P`，执行 **Tasks: Run Task**。最常用的任务是：

| 操作 | 任务或按键 | 结果 |
| --- | --- | --- |
| 编译 | `Build Debug` | 生成用于调试的固件。 |
| 烧录 | `Flash Debug (OpenOCD)` | 先编译，再下载到芯片、校验并复位。 |
| 调试 | 按 `F5`，选择 `Debug With OpenOCD` | 先编译，再下载程序，停在 `main` 附近。此时可单击行号左侧设置断点。 |
| 清理 | `CMake: Clean Debug` | 删除 Debug 构建目录，下次编译会重新生成。 |

安装了 `vscode-tasks` 后，状态栏会显示“配置、编译、烧录、清理”图标；鼠标悬停可看到说明。未安装该扩展时，上述任务仍可通过 **Tasks: Run Task** 正常执行。

## 2. 扩展介绍

本章面向已经知道工程、源文件和头文件用途，希望维护项目的人。

### 2.1 Debug 与 Release

- `Debug`：用于断点、单步和查看变量。默认推荐在日常开发中使用。
- `Release`：用于正式运行，通常会启用更高优化并减小固件体积，但不适合逐行调试。

通过 `CMake: Configure Debug`、`Build Debug` 构建 Debug；通过 `CMake: Configure Release`、`Build Release` 构建 Release。调试模板默认运行 Debug 固件。

### 2.2 添加 App、BSP、Mod 等用户代码

打开配置器后进入 **CMake 构建目标配置** 页：

1. 在“源文件 / 源目录”中选择一个工程分组，或新建虚拟分组。
2. 点击“添加文件”可选择一个或多个源文件；点击“添加源目录”会递归加入该目录中的 C、C++ 和汇编源文件。
3. 切换到“头文件目录”，将包含 `.h` 文件的目录加入编译器搜索路径。
4. 需要条件编译时，在“编译宏”中加入如 `USE_LOG` 或 `BOARD_REV=2`。
5. 使用预编译库时，在“链接目录”中加入库所在目录；库文件本身仍应按工程实际需求在 CMake 中处理。
6. 点击“生成”，然后执行一次 CMake Configure。

虚拟分组只用于配置器内整理项目，不会创建或移动磁盘上的目录。双击右侧列表是在修改 CMake 指向的路径或宏，不会重命名磁盘文件。点击一个已加入的源目录时，右侧只读显示该目录中的实际文件和子目录。

### 2.3 多人协作与 Git

每个人电脑上的工具路径可能不同，因此下面的本机文件不要提交到 Git：

```gitignore
.vscode/PathConfigurator.exe
.vscode/settings.json
.vscode/settings.json.bak
**/CMakeUserPresets.json
**/CMakeUserPresets.json.bak
```

下面的文件属于工程规则，应和源代码一起提交：

```text
.vscode/extensions.json
.vscode/launch.json
.vscode/settings.example.json
.vscode/tasks.json
<CMake 工程目录>/project-config.json
<CMake 工程目录>/cmake/PathConfiguratorProject.cmake
<CMake 工程目录>/cmake/PathConfiguratorCompilerCompat.cmake
```

工程中的 CMake 目录由 `.ioc` 的 `ProjectManager.ToolChainLocation` 决定。它可以叫 `ToolChain`、`CMake`，也可以直接是工程根目录；不要在任务或自己的代码中假定它一定名为 `ToolChain`。

### 2.4 复用本机工具路径

菜单 **配置** 提供三个选项：

- **保存为默认配置**：将 CMake、Ninja、st-arm-clang、GDB、OpenOCD 和调试接口保存到 `%LOCALAPPDATA%\PathConfigurator\user-settings.json`。
- **读取默认配置**：把已保存的通用工具路径填入当前工程。
- **从注册表检查 CLT 包**：尝试查找已安装的 STM32CubeCLT 并补齐可用路径。

默认配置不会保存 SVD、目标芯片配置、芯片型号或项目名，因为这些内容随工程而变化。工程尚未配置 SVD 时，读取默认配置或检测到 STM32CubeCLT 后会尝试自动匹配。

### 2.5 更新提示

程序窗口创建后会在后台静默检查 GitHub Releases。网络不可用、检查失败或当前已是最新版时不会打断操作；只有检测到新版本时，右上角才会出现红色版本提示。菜单 **其它 > 关于** 可查看当前版本、仓库地址并手动检查更新。

## 3. 细节讲解

本章面向需要确认工程改动范围和配置边界的专业使用者。

### 3.1 工程识别与路径优先级

启动后，程序从当前目录或 `/workspace` 指定目录向上查找 `.ioc`，并确认其中已选择 CMake 工具链且已生成 CMake 文件。随后按以下优先级读取工具路径；每个路径都会检查文件是否存在、文件名是否正确，并验证 OpenOCD 接口和目标芯片配置是否存在。

1. 当前工程 `.vscode/settings.json` 中的有效配置。
2. `%LOCALAPPDATA%\PathConfigurator\user-settings.json` 中的有效默认配置。使用前会询问。
3. Windows 注册表中检测到的 STM32CubeCLT。补充前会询问。
4. 当前窗口中手动选择的路径。

`settings.example.json` 是工程模板；首次选择“重建”时以它为基础创建 `settings.json`。后续选择“修改”时，只更新 `CustomCfg.*`、CMake Tools 所需的相关键和本机预设，同时保留用户加入的其它 VS Code 设置。

### 3.2 VS Code 配置键

模板的 `tasks.json`、`launch.json` 通过以下键读取配置器写入的路径：

| 键 | 用途 |
| --- | --- |
| `CustomCfg.cmakePath` | CMake 程序路径 |
| `CustomCfg.ninjaPath` | Ninja 程序路径 |
| `CustomCfg.starmClangPath` | ST 的 Clang 编译器路径 |
| `CustomCfg.gdbPath` | Cortex-Debug 使用的 GNU Arm GDB 路径 |
| `CustomCfg.openocdPath` | OpenOCD 程序路径 |
| `CustomCfg.openocd.interface` | OpenOCD 接口配置名，不含 `.cfg` |
| `CustomCfg.openocd.target` | OpenOCD 目标芯片配置名，包含 `.cfg` |
| `CustomCfg.svdFile` | Cortex-Debug 的 SVD 文件路径 |
| `CustomCfg.projectName` | CubeMX CMake 工程名 |
| `CustomCfg.toolchainLocation` | `.ioc` 中指定的 CMake 工程目录 |

配置器在 CMake 工程目录中创建本机 `CMakeUserPresets.json`，包含 `Debug-Local` 和 `Release-Local` 预设。这些预设将 CMake、Ninja、st-arm-clang 和 GDB 的本机路径传递给构建过程。CMake Tools 也会读取同一工程目录中的预设。

### 3.3 CMake 构建目标配置的输出

保存“CMake 构建目标配置”后，程序会在 `.ioc` 指定的 CMake 工程目录中更新：

```text
project-config.json
cmake/PathConfiguratorProject.cmake
CMakeLists.txt
```

`project-config.json` 保存源文件、源目录、头文件目录、编译宏、链接目录和虚拟分组；`PathConfiguratorProject.cmake` 将这些规则加入 CubeMX 的 CMake 目标。首次保存时，`CMakeLists.txt` 只增加一次 `include(...)`，以加载该模块。

源目录使用递归收集规则。目录内新增或删除源文件后，应重新执行 CMake Configure，再编译。

首次写入工具链配置时，还可能创建 `cmake/PathConfiguratorCompilerCompat.cmake` 并在 `CMakeLists.txt` 中增加一次引用。它仅为 st-arm-clang 处理 CubeMX H7 CMSIS 的兼容警告，不会修改 CubeMX 生成的头文件或源文件。

### 3.4 文件读写与安全边界

| 类别 | 文件或位置 |
| --- | --- |
| 读取 | `.ioc`、CMakeLists.txt、`.vscode/settings.json`、`.vscode/settings.example.json`、用户默认配置、STM32CubeCLT 注册表项 |
| 写入 | `.vscode/settings.json`、CMakeUserPresets.json、project-config.json、PathConfiguratorProject.cmake、PathConfiguratorCompilerCompat.cmake |
| 可能修改 | CMakeLists.txt，只插入配置器维护的唯一 `include(...)` 行 |
| 不会修改 | 系统环境变量、`.ioc`、CubeMX 生成的源文件和头文件、OpenOCD 配置文件、已烧录的固件 |

点击“重建”“修改”“生成”或“保存为默认配置”时，会先列出本次可能修改或创建的文件，并询问是否生成 `.bak` 备份。选择取消或关闭该提示窗口时，不会修改文件。

可选命令行用法：

```text
PathConfigurator.exe
PathConfigurator.exe /workspace <STM32 工程目录>
PathConfigurator.exe /validate <STM32 工程目录>
PathConfigurator.exe /help
```

`/validate` 只检查工程 `.vscode/settings.json`，不修改任何文件。

## 4. 常见问题

### 配置器提示“不是 CMake 工程”或“未生成 CMake 代码”

回到 STM32CubeMX 的 **Project Manager**，将 `Toolchain / IDE` 设为 `CMake`，然后重新点击 **Generate Code**。确认 VS Code 打开的是包含 `.ioc` 的最外层工程目录。

### 配置器中不知道每个路径该选什么

只选择 1.1 节表格中列出的 `.exe` 文件，不要选择文件夹。选择 STM32CubeCLT 中任意一个工具后，程序通常会识别安装包并询问是否自动补齐其它工具。OpenOCD 只需选择 `openocd.exe`。

### SVD 没有自动出现

先确认已选中的 CMake、Ninja、starm-clang 或 GDB 来自 STM32CubeCLT。重新选择其中任意一个文件后，配置器会重新尝试匹配。仍失败时，手动选择与 `.ioc` 芯片对应的 `.svd` 文件；SVD 不影响编译和烧录，只影响调试时的寄存器显示。

### 编译时提示 `Set STARM_CLANG_PATH` 或提示找不到 Ninja

重新运行配置器，确认选择的是 STM32CubeCLT 内的 `starm-clang.exe` 和 `ninja.exe`，第一次配置点击“重建”。之后执行 **Developer: Reload Window**，再运行 `Build Debug`。

### CMake 提示 `CMakeCache.txt directory is different` 或工程路径已经移动

运行 `CMake: Clean Debug`，然后重新运行 `Build Debug`。这会删除旧路径留下的 Debug 构建缓存，并在新目录重新配置。

### 烧录失败，提示找不到调试器、不能连接目标或无法识别芯片

依次检查：

1. 开发板是否已供电。
2. 调试器是否连接到电脑，Windows 是否识别它。
3. SWDIO、SWCLK、GND 是否正确连接；必要时连接 NRST。
4. 配置器中“调试接口”是否与实际硬件一致，例如 CMSIS-DAP 使用 `cmsis-dap.cfg`，ST-LINK 使用 `stlink.cfg`。
5. “目标芯片配置”是否和芯片系列一致。可点击选择按钮从 OpenOCD 的 target 配置中重新选择。

### 按 F5 后 Cortex-Debug 提示找不到 GDB、OpenOCD 或 ELF 文件

重新运行配置器并检查 GDB、OpenOCD 路径。随后先运行 `Build Debug`；构建成功后，确认 CMake 构建目录中已有 `<项目名>.elf`，再按 F5。不要手动改动 `launch.json` 中的 `CustomCfg.*` 引用。

### `#include "main.h"` 无法跳转或没有代码补全

安装 VS Code 的 C/C++ 扩展，先完成一次 `Build Debug` 或 CMake Configure，再执行 **Developer: Reload Window**。配置器会把 `compile_commands.json` 的位置写入 VS Code 配置，扩展随后会据此建立代码索引。

### 我想换电脑，是否要重新配置？

要。复制工程和模板即可，但新电脑仍需安装工具并运行一次 `Configure Local Toolchain (GUI)`。不要复制别人的 `settings.json` 或 `CMakeUserPresets.json`，因为里面保存的是对方电脑的绝对路径。

### 我只想修改路径，不想覆盖自己的 VS Code 设置

配置器存在现有 `settings.json` 时，使用“修改”。它会保留不属于配置器的个人设置；“重建”用于从 `settings.example.json` 重新建立本机配置。
