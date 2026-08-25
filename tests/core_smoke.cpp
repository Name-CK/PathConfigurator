#include "core.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {

bool WriteText(const std::wstring& path, const char* text) {
    std::error_code error;
    fs::create_directories(fs::path(path).parent_path(), error);
    if (error) return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const size_t length = std::strlen(text);
    const bool ok = WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr) && written == length;
    CloseHandle(file);
    return ok;
}

bool ReadText(const std::wstring& path, std::string& text) {
    text.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return false;
    }
    text.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = text.empty() || ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(file);
    text.resize(read);
    return ok;
}

int Fail(int code, const char* detail) {
    std::fprintf(stderr, "core_smoke failed (%d): %s\n", code, detail);
    return code;
}

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::wstring path) : path_(std::move(path)) {}

    bool Create() {
        std::error_code error;
        return fs::create_directories(path_, error) && !error;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const std::wstring& Path() const { return path_; }

private:
    std::wstring path_;
};

class EnvironmentOverride {
public:
    EnvironmentOverride(const wchar_t* name, const std::wstring& value) : name_(name) {
        const DWORD size = GetEnvironmentVariableW(name_, nullptr, 0);
        if (size > 0) {
            previous_.resize(size);
            if (GetEnvironmentVariableW(name_, previous_.data(), size) > 0) {
                previous_.resize(wcslen(previous_.c_str()));
                hadPrevious_ = true;
            }
        }
        active_ = SetEnvironmentVariableW(name_, value.c_str()) == TRUE;
    }

    ~EnvironmentOverride() {
        if (active_) SetEnvironmentVariableW(name_, hadPrevious_ ? previous_.c_str() : nullptr);
    }

    bool Active() const { return active_; }

private:
    const wchar_t* name_;
    std::wstring previous_;
    bool hadPrevious_ = false;
    bool active_ = false;
};

} // namespace

int main() {
    wchar_t tempPath[MAX_PATH]{};
    if (GetTempPathW(ARRAY_SIZE(tempPath), tempPath) == 0) return Fail(1, "cannot locate the temporary directory");
    const std::wstring fixturePath = pathconfig::JoinPath(
        tempPath, L"PathConfiguratorCoreTest-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    TemporaryDirectory fixture(fixturePath);
    if (!fixture.Create()) return Fail(2, "cannot create the test fixture");

    const std::wstring cmakeRoot = pathconfig::JoinPath(fixture.Path(), L"GeneratedCMake");
    const std::wstring appSource = pathconfig::JoinPath(fixture.Path(), L"App\\Src\\app.c");
    const std::wstring appInclude = pathconfig::JoinPath(fixture.Path(), L"App\\Inc");
    const std::wstring bspSource = pathconfig::JoinPath(fixture.Path(), L"BSP\\Src\\driver.c");
    const std::wstring libraryDirectory = pathconfig::JoinPath(fixture.Path(), L"Middlewares\\Library");
    if (!WriteText(pathconfig::JoinPath(fixture.Path(), L"PortableProject.ioc"),
                   "Mcu.CPN=STM32F407VET6\r\nProjectManager.TargetToolchain=CMake\r\nProjectManager.ToolChainLocation=GeneratedCMake\r\n") ||
        !WriteText(pathconfig::JoinPath(cmakeRoot, L"CMakeLists.txt"), "set(CMAKE_PROJECT_NAME PortableProject)\n") ||
        !WriteText(appSource, "void app(void) {}\n") || !WriteText(bspSource, "void driver(void) {}\n") ||
        !WriteText(pathconfig::JoinPath(appInclude, L"app.h"), "#pragma once\n") ||
        !WriteText(pathconfig::JoinPath(libraryDirectory, L"placeholder.txt"), "fixture\n")) {
        return Fail(3, "cannot create the portable STM32 project fixture");
    }

    const std::wstring cubeRoot = pathconfig::JoinPath(fixture.Path(), L"STM32CubeCLT");
    const std::wstring openOcdRoot = pathconfig::JoinPath(fixture.Path(), L"OpenOCD");
    pathconfig::ToolPaths tools;
    tools.cmake = pathconfig::JoinPath(cubeRoot, L"CMake\\bin\\cmake.exe");
    tools.ninja = pathconfig::JoinPath(cubeRoot, L"Ninja\\bin\\ninja.exe");
    tools.starmClang = pathconfig::JoinPath(cubeRoot, L"st-arm-clang\\bin\\starm-clang.exe");
    tools.gdb = pathconfig::JoinPath(cubeRoot, L"GNU-tools-for-STM32\\bin\\arm-none-eabi-gdb.exe");
    tools.openocd = pathconfig::JoinPath(openOcdRoot, L"bin\\openocd.exe");
    tools.openocdInterface = L"cmsis-dap";
    tools.openocdTarget = L"stm32f4x.cfg";
    tools.svd = pathconfig::JoinPath(cubeRoot, L"STMicroelectronics_CMSIS_SVD\\STM32F407.svd");
    if (!WriteText(pathconfig::JoinPath(cubeRoot, L"STM32CubeCLT_metadata.bat"), "@rem fixture\n") ||
        !WriteText(tools.cmake, "") || !WriteText(tools.ninja, "") || !WriteText(tools.starmClang, "") ||
        !WriteText(tools.gdb, "") || !WriteText(tools.svd, "") || !WriteText(tools.openocd, "") ||
        !WriteText(pathconfig::JoinPath(openOcdRoot, L"scripts\\interface\\cmsis-dap.cfg"), "# fixture\n") ||
        !WriteText(pathconfig::JoinPath(openOcdRoot, L"scripts\\target\\stm32f4x.cfg"), "# fixture\n") ||
        !WriteText(pathconfig::JoinPath(openOcdRoot, L"scripts\\target\\stm32h7x.cfg"), "# fixture\n")) {
        return Fail(4, "cannot create the portable toolchain fixture");
    }

    pathconfig::WorkspaceInfo workspace;
    std::wstring error;
    if (!pathconfig::LoadWorkspace(fixture.Path(), workspace, error)) return Fail(5, "cannot load the fixture workspace");
    if (workspace.toolchainLocation != L"GeneratedCMake" || workspace.projectName != L"PortableProject")
        return Fail(6, "workspace metadata was not parsed correctly");
    if (workspace.chipType.find(L"STM32F407") == std::wstring::npos ||
        pathconfig::FindWorkspaceRoot(pathconfig::JoinPath(fixture.Path(), L".vscode")) != workspace.root)
        return Fail(7, "workspace discovery failed");

    pathconfig::ToolPaths cubeTools;
    std::wstring detectedCubeRoot, report;
    if (!pathconfig::DetectCubeClt(tools.starmClang, cubeTools, detectedCubeRoot, report) || detectedCubeRoot != cubeRoot ||
        cubeTools.cmake != tools.cmake || cubeTools.gdb != tools.gdb)
        return Fail(8, "CubeCLT detection failed");
    if (pathconfig::FindSvdForChip(cubeRoot, L"STM32F407VET6") != tools.svd ||
        pathconfig::FindOpenOcdTargetForChip(tools.openocd, L"STM32F407VET6") != L"stm32f4x.cfg" ||
        pathconfig::FindOpenOcdTargetForChip(tools.openocd, L"STM32H723ZET6") != L"stm32h7x.cfg")
        return Fail(9, "SVD or OpenOCD target detection failed");
    if (!pathconfig::ValidateTools(tools, true).ok) return Fail(10, "valid fixture tools failed validation");

    const std::wstring settingsPath = pathconfig::JoinPath(fixture.Path(), L".vscode\\settings.json");
    if (!WriteText(settingsPath, "{\n  \"CustomCfg.openocdScripts\": \"obsolete\",\n  \"personal.setting\": \"keep\"\n}\n"))
        return Fail(11, "cannot create settings fixture");
    if (!pathconfig::WriteConfiguration(workspace, tools, workspace.projectName, workspace.chipType, tools.svd, false, error))
        return Fail(12, "cannot write portable VS Code configuration");
    pathconfig::ToolPaths writtenTools;
    std::wstring writtenProject, writtenChip, writtenSvd;
    std::string writtenSettings;
    if (!pathconfig::LoadSettings(settingsPath, writtenTools, writtenProject, writtenChip, writtenSvd, writtenSettings) ||
        writtenTools.openocdInterface != L"cmsis-dap" || writtenTools.openocdTarget != L"stm32f4x.cfg" ||
        writtenSettings.find("personal.setting") == std::string::npos || writtenSettings.find("CustomCfg.openocdScripts") != std::string::npos)
        return Fail(13, "settings preservation or migration failed");

    std::wstring relativeSource;
    if (!pathconfig::MakeToolchainRelativePath(workspace, appSource, false, relativeSource) || relativeSource != L"../App/Src/app.c")
        return Fail(14, "source path was not made portable");
    pathconfig::CMakeTargetConfig targetConfig;
    targetConfig.sources.push_back({relativeSource, false});
    targetConfig.sources.push_back({L"../BSP/Src", true});
    targetConfig.includeDirectories.push_back(L"../App/Inc");
    targetConfig.compileDefinitions.push_back(L"USE_BSP");
    targetConfig.linkDirectories.push_back(L"../Middlewares/Library");
    if (!pathconfig::WriteCMakeTargetConfig(workspace, targetConfig, error)) return Fail(15, "cannot write CMake target configuration");
    pathconfig::CMakeTargetConfig loadedTargetConfig;
    std::string generatedModule, updatedCmakeLists;
    if (!pathconfig::LoadCMakeTargetConfig(workspace, loadedTargetConfig, error) || loadedTargetConfig.sources.size() != 2 ||
        !loadedTargetConfig.sources[1].isFolder || !ReadText(workspace.cmakeTargetModulePath, generatedModule) ||
        !ReadText(pathconfig::JoinPath(cmakeRoot, L"CMakeLists.txt"), updatedCmakeLists) ||
        generatedModule.find("GLOB_RECURSE") == std::string::npos || generatedModule.find("CONFIGURE_DEPENDS") == std::string::npos ||
        updatedCmakeLists.find("PathConfiguratorProject.cmake") == std::string::npos)
        return Fail(16, "CMake target configuration generation failed");

    const std::wstring rootCmakeProject = pathconfig::JoinPath(fixture.Path(), L"EmptyToolchainLocation");
    if (!WriteText(pathconfig::JoinPath(rootCmakeProject, L"RootProject.ioc"),
                   "Mcu.CPN=STM32F407VET6\r\nProjectManager.TargetToolchain=CMake\r\nProjectManager.ToolChainLocation=\r\n") ||
        !WriteText(pathconfig::JoinPath(rootCmakeProject, L"CMakeLists.txt"), "set(CMAKE_PROJECT_NAME RootProject)\n"))
        return Fail(17, "cannot create empty-location project fixture");
    pathconfig::WorkspaceInfo rootWorkspace;
    if (!pathconfig::LoadWorkspace(rootCmakeProject, rootWorkspace, error) || rootWorkspace.toolchainLocation != L".")
        return Fail(18, "empty ToolChainLocation was not handled");

    const std::wstring invalidSettingsPath = pathconfig::JoinPath(fixture.Path(), L".vscode\\invalid-settings.json");
    if (!WriteText(invalidSettingsPath, "{\n  \"CustomCfg.cmakePath\": \"C:\\\\missing\\\\cmake.exe\",\n  \"CustomCfg.openocdPath\": \"C:\\\\missing\\\\openocd.exe\"\n}\n"))
        return Fail(19, "cannot create invalid settings fixture");
    pathconfig::ToolPaths invalidTools;
    std::wstring ignoredProject, ignoredChip, ignoredSvd;
    std::string ignoredSettings;
    if (!pathconfig::LoadSettings(invalidSettingsPath, invalidTools, ignoredProject, ignoredChip, ignoredSvd, ignoredSettings) ||
        !invalidTools.cmake.empty() || !invalidTools.openocd.empty())
        return Fail(20, "invalid tool paths were not discarded");

    EnvironmentOverride localAppData(L"LOCALAPPDATA", fixture.Path());
    if (!localAppData.Active()) return Fail(21, "cannot redirect LOCALAPPDATA for the test");
    pathconfig::ToolPaths defaultTools = tools;
    defaultTools.svd.clear();
    defaultTools.openocdTarget.clear();
    if (!pathconfig::WriteUserDefaultSettings(defaultTools, error)) return Fail(22, "cannot write default settings");
    pathconfig::ToolPaths loadedDefaults;
    if (!pathconfig::LoadUserDefaultSettings(loadedDefaults, error) || !loadedDefaults.svd.empty() || !loadedDefaults.openocdTarget.empty() ||
        loadedDefaults.cmake != tools.cmake || loadedDefaults.openocd != tools.openocd)
        return Fail(23, "default settings are not project independent");

    std::wprintf(L"core_smoke passed: %ls\n", workspace.projectName.c_str());
    return 0;
}
