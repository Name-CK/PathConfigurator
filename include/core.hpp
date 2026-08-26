#pragma once

#include <windows.h>
#include <string>
#include <vector>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

namespace pathconfig {

struct ToolPaths {
    std::wstring cmake;
    std::wstring ninja;
    std::wstring starmClang;
    std::wstring gdb;
    std::wstring openocd;
    std::wstring openocdInterface;
    std::wstring openocdTarget;
    std::wstring svd;
};

// CMake 构建目标配置中的源文件既可以是单个文件，也可以是递归收集的源目录。
// virtualFolder 仅用于配置器的虚拟文件夹，不会创建磁盘目录，也不会改变 CMake 编译规则。
struct TargetSourceEntry {
    std::wstring path;
    bool isFolder = false;
    std::wstring virtualFolder;
};

// 这些内容属于工程构建规则，应与 project-config.json 一起提交到 Git。
struct CMakeTargetConfig {
    // 用 / 分隔的虚拟文件夹路径，例如 App/Protocol。空根目录不保存到此列表。
    std::vector<std::wstring> virtualFolders;
    std::vector<TargetSourceEntry> sources;
    std::vector<std::wstring> includeDirectories;
    std::vector<std::wstring> compileDefinitions;
    std::vector<std::wstring> linkDirectories;
};

struct WorkspaceInfo {
    std::wstring root;
    std::wstring toolchainRoot;
    std::wstring toolchainLocation;
    std::wstring projectName;
    std::wstring chipType;
    std::wstring projectLabel;
    std::wstring settingsPath;
    std::wstring examplePath;
    std::wstring presetsPath;
    std::wstring cmakeTargetConfigPath;
    std::wstring cmakeTargetModulePath;
    bool hasExample = false;
};

struct ValidationResult {
    bool ok = false;
    std::wstring message;
};

std::wstring Trim(const std::wstring& value);
std::wstring NormalizePath(const std::wstring& value);
bool FileExists(const std::wstring& path);
bool FolderExists(const std::wstring& path);
std::wstring JoinPath(const std::wstring& left, const std::wstring& right);
std::wstring GetParentPath(const std::wstring& path);
std::wstring FileName(const std::wstring& path);
// 将用户选择的文件或目录转换为相对于 .ioc 所指定 CMake 目录的可移植路径。
bool MakeToolchainRelativePath(const WorkspaceInfo& info, const std::wstring& selectedPath,
                               bool isDirectory, std::wstring& relativePath);

bool LoadWorkspace(const std::wstring& root, WorkspaceInfo& info, std::wstring& error);
// 从当前目录向上查找已生成 CMake 代码的 STM32CubeMX 工程根目录。
std::wstring FindWorkspaceRoot(const std::wstring& start);
bool LoadSettings(const std::wstring& path, ToolPaths& tools, std::wstring& projectName,
                 std::wstring& chipType, std::wstring& svd, std::string& document);
// 清理读取到的无效文件路径，并在 OpenOCD scripts 可用时检查 interface/target 配置文件。
void SanitizeToolPaths(ToolPaths& tools);
ValidationResult ValidateTools(const ToolPaths& tools, bool requireSvd);
// 检查配置器生成的 Debug/Release 本机预设是否仍与当前工具链一致；忽略用户额外添加的预设。
bool IsCMakeUserPresetsCurrent(const WorkspaceInfo& info, const ToolPaths& tools);
// 本机默认配置：%LOCALAPPDATA%\PathConfigurator\user-settings.json。
// 仅保存 CMake、Ninja、starm-clang、GNU Arm GDB、OpenOCD 和调试接口，不保存 SVD 或目标芯片配置。
std::wstring GetUserDefaultSettingsPath();
bool LoadUserDefaultSettings(ToolPaths& tools, std::wstring& error);
bool WriteUserDefaultSettings(const ToolPaths& tools, std::wstring& error, bool createBackup = false);
bool DetectCubeClt(const std::wstring& selectedPath, ToolPaths& tools, std::wstring& cubeRoot,
                   std::wstring& report);
// 从系统注册表查找 STM32CubeCLT，并只返回实际存在的工具路径。
bool DetectCubeCltFromRegistry(ToolPaths& tools, std::wstring& cubeRoot,
                               std::wstring& report);
bool DetectOpenOcdScripts(const std::wstring& openocdPath, std::wstring& scriptsPath);
// 根据 .ioc 芯片型号，在 OpenOCD 实际提供的 target 配置中选择最匹配的 .cfg 文件名。
std::wstring FindOpenOcdTargetForChip(const std::wstring& openocdPath, const std::wstring& chipType);
std::wstring FindSvdForChip(const std::wstring& root, const std::wstring& chipType);
// 在 .ioc 所指定的 CMake 目录读写 project-config.json，并生成 cmake/PathConfiguratorProject.cmake。
// 保存时会在 CubeMX 的 CMakeLists.txt 中补入唯一的 include(...) 接入行。
bool LoadCMakeTargetConfig(const WorkspaceInfo& info, CMakeTargetConfig& config, std::wstring& error);
bool WriteCMakeTargetConfig(const WorkspaceInfo& info, const CMakeTargetConfig& config, std::wstring& error,
                            bool createBackup = false);
bool WriteConfiguration(const WorkspaceInfo& info, const ToolPaths& tools, const std::wstring& projectName,
                        const std::wstring& chipType, const std::wstring& svd, bool fromExample,
                        std::wstring& error, bool createBackup = false);

} // namespace pathconfig
