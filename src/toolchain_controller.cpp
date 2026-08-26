#include "toolchain_controller.hpp"

#include "core.hpp"

#include <commdlg.h>

#include <vector>

namespace pathconfig::toolchain {
namespace {

const wchar_t* const kFileNames[] = {
    L"cmake.exe", L"ninja.exe", L"starm-clang.exe", L"arm-none-eabi-gdb.exe",
    L"openocd.exe", L"", L"", L".svd"
};

void SetStatus(gui::AppState& app, const std::wstring& text)
{
    if (app.status) SetWindowTextW(app.status, text.c_str());
}

std::wstring GetEdit(HWND control)
{
    if (!control) return {};
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return Trim(value);
}

void SetEdit(HWND control, const std::wstring& value)
{
    if (control) SetWindowTextW(control, value.c_str());
}

std::wstring InterfaceNameFromControl(HWND control)
{
    std::wstring value = GetEdit(control);
    constexpr wchar_t extension[] = L".cfg";
    constexpr size_t extensionLength = ARRAY_SIZE(extension) - 1;
    if (value.size() > extensionLength &&
        _wcsicmp(value.c_str() + value.size() - extensionLength, extension) == 0)
        value.resize(value.size() - extensionLength);
    return value;
}

void PopulateOpenOcdInterfaceCombo(gui::AppState& app)
{
    HWND combo = app.edits[5];
    if (!combo) return;

    std::wstring desired = InterfaceNameFromControl(combo);
    if (desired.empty()) desired = app.tools.openocdInterface;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    std::wstring scripts;
    const bool hasScripts = app.edits[4] && DetectOpenOcdScripts(GetEdit(app.edits[4]), scripts);
    if (hasScripts)
    {
        const wchar_t* names[] = {L"cmsis-dap", L"stlink"};
        for (const wchar_t* name : names)
        {
            if (FileExists(JoinPath(scripts, std::wstring(L"interface\\") + name + L".cfg")))
                SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((std::wstring(name) + L".cfg").c_str()));
        }
        if (!desired.empty() &&
            FileExists(JoinPath(scripts, std::wstring(L"interface\\") + desired + L".cfg")) &&
            SendMessageW(combo, CB_FINDSTRINGEXACT, -1, reinterpret_cast<LPARAM>((desired + L".cfg").c_str())) == CB_ERR)
        {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((desired + L".cfg").c_str()));
        }
    }

    int selected = CB_ERR;
    if (!desired.empty())
        selected = static_cast<int>(SendMessageW(combo, CB_FINDSTRINGEXACT, -1, reinterpret_cast<LPARAM>((desired + L".cfg").c_str())));
    if (selected == CB_ERR)
        selected = static_cast<int>(SendMessageW(combo, CB_FINDSTRINGEXACT, -1, reinterpret_cast<LPARAM>(L"cmsis-dap.cfg")));
    if (selected == CB_ERR && SendMessageW(combo, CB_GETCOUNT, 0, 0) > 0)
        selected = 0;
    if (selected != CB_ERR) SendMessageW(combo, CB_SETCURSEL, selected, 0);
    EnableWindow(combo, hasScripts && selected != CB_ERR);
}

bool SamePath(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(NormalizePath(left).c_str(), NormalizePath(right).c_str()) == 0;
}

bool SameSharedTools(const ToolPaths& left, const ToolPaths& right)
{
    return SamePath(left.cmake, right.cmake) && SamePath(left.ninja, right.ninja) &&
        SamePath(left.starmClang, right.starmClang) && SamePath(left.gdb, right.gdb) &&
        SamePath(left.openocd, right.openocd) &&
        _wcsicmp(Trim(left.openocdInterface).c_str(), Trim(right.openocdInterface).c_str()) == 0;
}

bool HasAnySharedTool(const ToolPaths& tools)
{
    return !tools.cmake.empty() || !tools.ninja.empty() || !tools.starmClang.empty() ||
        !tools.gdb.empty() || !tools.openocd.empty() || !tools.openocdInterface.empty();
}

bool HasCompleteSharedToolchain(const ToolPaths& tools)
{
    return !tools.cmake.empty() && !tools.ninja.empty() && !tools.starmClang.empty() &&
        !tools.gdb.empty() && !tools.openocd.empty();
}

void MergeSharedTools(ToolPaths& destination, const ToolPaths& source, bool overwrite)
{
    std::wstring* destinationValues[] = {
        &destination.cmake, &destination.ninja, &destination.starmClang, &destination.gdb,
        &destination.openocd, &destination.openocdInterface};
    const std::wstring* sourceValues[] = {
        &source.cmake, &source.ninja, &source.starmClang, &source.gdb,
        &source.openocd, &source.openocdInterface};
    for (size_t i = 0; i < ARRAY_SIZE(destinationValues); ++i)
        if (!sourceValues[i]->empty() && (overwrite || destinationValues[i]->empty()))
            *destinationValues[i] = *sourceValues[i];
}

std::wstring DescribeSharedTools(const ToolPaths& tools)
{
    struct Entry { const wchar_t* name; const std::wstring* path; } entries[] = {
        {L"CMake", &tools.cmake}, {L"Ninja", &tools.ninja}, {L"starm-clang", &tools.starmClang},
        {L"GNU Arm GDB", &tools.gdb}, {L"OpenOCD", &tools.openocd}, {L"调试接口", &tools.openocdInterface}};
    std::wstring text;
    for (const Entry& entry : entries)
        if (!entry.path->empty()) text += std::wstring(entry.name) + L"：" + *entry.path + L"\r\n";
    return text.empty() ? L"（没有有效的默认工具路径）\r\n" : text;
}

enum class BackupChoice { Cancel, Skip, Create };

BackupChoice ConfirmBackups(HWND owner, const std::vector<std::wstring>& candidates)
{
    std::vector<std::wstring> files;
    for (const std::wstring& candidate : candidates)
    {
        if (candidate.empty()) continue;
        bool alreadyListed = false;
        for (const std::wstring& existing : files)
            if (SamePath(existing, candidate)) { alreadyListed = true; break; }
        if (!alreadyListed) files.push_back(candidate);
    }
    if (files.empty()) return BackupChoice::Skip;

    std::wstring message = L"本次操作将会修改/创建以下文件，是否进行备份：\r\n\r\n";
    for (const std::wstring& file : files) message += file + L"\r\n";
    const int choice = MessageBoxW(owner, message.c_str(), L"生成文件备份",
        MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_TOPMOST);
    if (choice == IDYES) return BackupChoice::Create;
    return choice == IDNO ? BackupChoice::Skip : BackupChoice::Cancel;
}

std::wstring SelectFile(HWND owner, const wchar_t* expectedName)
{
    wchar_t path[4096]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(ARRAY_SIZE(path));
    const bool isSvd = _wcsicmp(expectedName, L".svd") == 0;
    std::wstring filter = isSvd ? L"SVD 文件 (*.svd)" : expectedName;
    filter.push_back(L'\0');
    filter += isSvd ? L"*.svd" : expectedName;
    filter.push_back(L'\0');
    filter += L"所有文件";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    const std::wstring title = std::wstring(L"选择 ") + expectedName;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrTitle = title.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    const bool selected = GetOpenFileNameW(&ofn) == TRUE;
    if (owner) InvalidateRect(owner, nullptr, FALSE);
    return selected ? NormalizePath(path) : std::wstring{};
}

std::wstring SelectOpenOcdConfig(HWND owner, const std::wstring& openocdPath, const wchar_t* folder, const wchar_t* title)
{
    std::wstring scripts;
    if (!DetectOpenOcdScripts(openocdPath, scripts))
    {
        MessageBoxW(owner, L"无法从当前 openocd.exe 的安装目录找到 scripts 文件夹。\r\n请先选择有效的 OpenOCD 程序。",
            L"无法选择 OpenOCD 配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return {};
    }
    const std::wstring configFolder = JoinPath(scripts, folder);
    if (!FolderExists(configFolder))
    {
        MessageBoxW(owner, (L"未找到 OpenOCD 配置目录：\r\n" + configFolder).c_str(),
            L"无法选择 OpenOCD 配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return {};
    }
    wchar_t path[4096]{};
    OPENFILENAMEW ofn{};
    const wchar_t filter[] = L"OpenOCD 配置文件 (*.cfg)\0*.cfg\0所有文件\0*.*\0\0";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(ARRAY_SIZE(path));
    ofn.lpstrInitialDir = configFolder.c_str();
    ofn.lpstrFilter = filter;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    const bool selected = GetOpenFileNameW(&ofn) == TRUE;
    if (owner) InvalidateRect(owner, nullptr, FALSE);
    return selected ? NormalizePath(path) : std::wstring{};
}

bool ShouldUpdateUserDefaults(gui::AppState& app, HWND owner)
{
    const ToolPaths defaults = SharedTools(app.tools);
    ToolPaths cached;
    std::wstring error;
    const bool hasCache = LoadUserDefaultSettings(cached, error);
    if (hasCache && SameSharedTools(defaults, cached)) return false;
    const std::wstring prompt = L"当前工程的共享工具路径与默认配置不同。\r\n\r\n是否更新本机默认配置？\r\n\r\n" +
        DescribeSharedTools(defaults) + L"\r\n不会保存 SVD 或目标芯片配置。";
    return MessageBoxW(owner, prompt.c_str(), L"更新默认配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES;
}

} // namespace

const std::wstring& InitialValue(const gui::AppState& app, int index)
{
    switch (index)
    {
    case 0: return app.tools.cmake;
    case 1: return app.tools.ninja;
    case 2: return app.tools.starmClang;
    case 3: return app.tools.gdb;
    case 4: return app.tools.openocd;
    case 5: return app.tools.openocdInterface;
    case 6: return app.tools.openocdTarget;
    default: return app.svd;
    }
}

void PullControls(gui::AppState& app)
{
    app.tools.cmake = GetEdit(app.edits[0]);
    app.tools.ninja = GetEdit(app.edits[1]);
    app.tools.starmClang = GetEdit(app.edits[2]);
    app.tools.gdb = GetEdit(app.edits[3]);
    app.tools.openocd = GetEdit(app.edits[4]);
    app.tools.openocdInterface = InterfaceNameFromControl(app.edits[5]);
    app.tools.openocdTarget = GetEdit(app.edits[6]);
    app.svd = GetEdit(app.edits[7]);
    app.tools.svd = app.svd;
}

void PushControls(gui::AppState& app)
{
    SetEdit(app.edits[0], app.tools.cmake);
    SetEdit(app.edits[1], app.tools.ninja);
    SetEdit(app.edits[2], app.tools.starmClang);
    SetEdit(app.edits[3], app.tools.gdb);
    SetEdit(app.edits[4], app.tools.openocd);
    SetEdit(app.edits[6], app.tools.openocdTarget);
    SetEdit(app.edits[7], app.svd);
    UpdateOpenOcdConfigControls(app);
    app.tools.svd = app.svd;
}

void UpdateOpenOcdConfigControls(gui::AppState& app)
{
    const bool enabled = app.edits[4] && FileExists(GetEdit(app.edits[4]));
    PopulateOpenOcdInterfaceCombo(app);
    if (app.edits[6]) EnableWindow(app.edits[6], enabled);
    for (HWND button : app.openocdConfigBrowse)
        if (button) EnableWindow(button, enabled);
}

ToolPaths SharedTools(const ToolPaths& tools)
{
    ToolPaths shared = tools;
    shared.svd.clear();
    shared.openocdTarget.clear();
    return shared;
}

std::vector<std::wstring> ConfigurationFiles(const WorkspaceInfo& workspace)
{
    return {
        workspace.settingsPath,
        workspace.presetsPath,
        JoinPath(workspace.toolchainRoot, L"cmake\\PathConfiguratorCompilerCompat.cmake"),
        JoinPath(workspace.toolchainRoot, L"CMakeLists.txt")
    };
}

void ApplyOpenOcdDefaults(gui::AppState& app)
{
    if (FileExists(app.tools.openocd) && app.tools.openocdInterface.empty())
    {
        app.tools.openocdInterface = L"cmsis-dap";
        SanitizeToolPaths(app.tools);
    }
    if (app.tools.openocdTarget.empty())
        app.tools.openocdTarget = FindOpenOcdTargetForChip(app.tools.openocd, app.chipType);
}

bool TryAutoFillSvd(gui::AppState& app)
{
    if (!app.svd.empty() && FileExists(app.svd)) return false;
    const std::wstring* candidates[] = {&app.tools.cmake, &app.tools.ninja, &app.tools.starmClang, &app.tools.gdb};
    for (const std::wstring* candidate : candidates)
    {
        if (candidate->empty()) continue;
        ToolPaths detected;
        std::wstring cubeRoot, report;
        if (!DetectCubeClt(*candidate, detected, cubeRoot, report)) continue;
        const std::wstring svd = FindSvdForChip(cubeRoot, app.chipType);
        if (!svd.empty() && FileExists(svd))
        {
            app.svd = svd;
            app.tools.svd = svd;
            return true;
        }
    }
    return false;
}

void OfferRegistryTools(gui::AppState& app, HWND owner, bool showMissing)
{
    ToolPaths registryTools;
    std::wstring registryRoot, registryReport;
    if (!DetectCubeCltFromRegistry(registryTools, registryRoot, registryReport))
    {
        if (showMissing)
            MessageBoxW(owner, L"未在注册表中找到有效的 STM32CubeCLT 安装记录。", L"检查 STM32CubeCLT", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    SanitizeToolPaths(registryTools);
    const std::wstring registrySvd = FindSvdForChip(registryRoot, app.chipType);
    if (!registrySvd.empty()) registryReport += L"SVD：" + registrySvd + L"\r\n";
    const std::wstring prompt = L"检测到注册表中的 STM32CubeCLT 工具：\r\n\r\n" + registryReport +
        L"\r\n是否补充到当前配置界面？\r\n已有的有效工具路径不会被覆盖。";
    if (MessageBoxW(owner, prompt.c_str(), L"检查 STM32CubeCLT", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES) return;
    MergeSharedTools(app.tools, registryTools, false);
    if (app.svd.empty() && !registrySvd.empty()) app.svd = registrySvd;
    app.tools.svd = app.svd;
    ApplyOpenOcdDefaults(app);
    PushControls(app);
    SetStatus(app, L"已补充注册表检测到的 STM32CubeCLT 工具路径。");
}

void OfferUserDefaultsOnStartup(gui::AppState& app, HWND owner, bool hasProjectSettings)
{
    if (HasCompleteSharedToolchain(app.tools)) return;
    ToolPaths defaults;
    std::wstring error;
    if (!LoadUserDefaultSettings(defaults, error) || !HasAnySharedTool(defaults))
    {
        OfferRegistryTools(app, owner, false);
        return;
    }
    const std::wstring reason = hasProjectSettings
        ? L"工程 .vscode/settings.json 中存在缺失或无效的工具路径。"
        : L"未检测到可用的工程 .vscode/settings.json。";
    const std::wstring prompt = reason + L"\r\n\r\n是否使用已有默认配置补充当前工程？\r\n\r\n" +
        DescribeSharedTools(defaults) + L"\r\n工程中已有的有效路径不会被覆盖。";
    if (MessageBoxW(owner, prompt.c_str(), L"读取默认配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES) return;
    MergeSharedTools(app.tools, defaults, false);
    ApplyOpenOcdDefaults(app);
    if (!HasCompleteSharedToolchain(app.tools))
    {
        MessageBoxW(owner, L"默认配置未能补齐全部工具路径，将继续检查注册表中的 STM32CubeCLT。",
            L"默认配置不完整", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        OfferRegistryTools(app, owner, false);
    }
}

void ReadUserDefaults(gui::AppState& app, HWND owner)
{
    PullControls(app);
    ToolPaths defaults;
    std::wstring error;
    if (!LoadUserDefaultSettings(defaults, error))
    {
        MessageBoxW(owner, error.c_str(), L"读取默认配置", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        return;
    }
    if (!HasAnySharedTool(defaults))
    {
        MessageBoxW(owner, L"默认配置中没有有效的工具路径。", L"读取默认配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return;
    }
    const std::wstring prompt = L"是否读取下列默认配置？\r\n\r\n" + DescribeSharedTools(defaults) +
        L"\r\n这会替换界面中的共享工具路径；不会修改 SVD 或目标芯片配置。";
    if (MessageBoxW(owner, prompt.c_str(), L"读取默认配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES) return;
    MergeSharedTools(app.tools, defaults, true);
    ApplyOpenOcdDefaults(app);
    const bool foundSvd = TryAutoFillSvd(app);
    PushControls(app);
    SetStatus(app, foundSvd ? L"已读取默认配置，并从 STM32CubeCLT 自动匹配当前芯片的 SVD 文件。"
        : L"已读取默认配置；未找到当前芯片的 SVD，保留现有 SVD 与目标芯片配置。");
}

void SaveUserDefaults(gui::AppState& app, HWND owner)
{
    PullControls(app);
    SanitizeToolPaths(app.tools);
    app.svd = app.tools.svd;
    const ToolPaths defaults = SharedTools(app.tools);
    const ValidationResult validation = ValidateTools(defaults, false);
    if (!validation.ok)
    {
        MessageBoxW(owner, (L"默认配置需要完整且有效的共享工具路径：\r\n" + validation.message).c_str(),
            L"保存默认配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return;
    }
    const BackupChoice backup = ConfirmBackups(owner, {GetUserDefaultSettingsPath()});
    if (backup == BackupChoice::Cancel)
    {
        SetStatus(app, L"已取消保存默认配置。");
        return;
    }
    std::wstring error;
    if (!WriteUserDefaultSettings(defaults, error, backup == BackupChoice::Create))
    {
        MessageBoxW(owner, error.c_str(), L"保存默认配置", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return;
    }
    SetStatus(app, L"已保存本机默认工具配置。");
}

void Browse(gui::AppState& app, HWND owner, int index)
{
    if (index < 0 || index >= static_cast<int>(ARRAY_SIZE(kFileNames))) return;
    if (index == 5 || index == 6)
    {
        PullControls(app);
        const bool isInterfaceConfig = index == 5;
        const std::wstring selected = SelectOpenOcdConfig(owner, app.tools.openocd,
            isInterfaceConfig ? L"interface" : L"target",
            isInterfaceConfig ? L"选择 OpenOCD 调试接口配置" : L"选择 OpenOCD 目标芯片配置");
        if (selected.empty()) return;
        std::wstring value = FileName(selected);
        if (isInterfaceConfig)
        {
            if (value.size() > 4 && _wcsicmp(value.c_str() + value.size() - 4, L".cfg") == 0)
                value.resize(value.size() - 4);
            app.tools.openocdInterface = value;
            PopulateOpenOcdInterfaceCombo(app);
        }
        else
            SetEdit(app.edits[index], value);
        return;
    }

    const std::wstring selected = SelectFile(owner, kFileNames[index]);
    if (selected.empty()) return;
    SetEdit(app.edits[index], selected);
    if (index == 4 && GetEdit(app.edits[6]).empty())
    {
        const std::wstring target = FindOpenOcdTargetForChip(selected, app.chipType);
        if (!target.empty())
        {
            SetEdit(app.edits[6], target);
            SetStatus(app, L"已根据芯片型号选择 OpenOCD 目标芯片配置默认文件。");
        }
    }
    if (index == 4) UpdateOpenOcdConfigControls(app);
    if (index > 2) return;

    PullControls(app);
    ToolPaths detected = app.tools;
    std::wstring cubeRoot, report;
    if (!DetectCubeClt(GetEdit(app.edits[index]), detected, cubeRoot, report)) return;
    const std::wstring svd = FindSvdForChip(cubeRoot, app.chipType);
    if (!svd.empty()) report += L"SVD = " + svd + L"\r\n";
    const std::wstring message = L"检测到 STM32CubeCLT 工具包：\r\n" + cubeRoot + L"\r\n\r\n" + report + L"\r\n是否自动填充检测到的地址？";
    if (MessageBoxW(owner, message.c_str(), L"检测 STM32CubeCLT", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES)
    {
        app.tools = detected;
        if (!svd.empty()) app.svd = svd;
        PushControls(app);
        SetStatus(app, L"已从 STM32CubeCLT 自动填充工具链地址。");
    }
}

bool SaveConfiguration(gui::AppState& app, HWND owner, bool fromExample)
{
    PullControls(app);
    app.tools.cmake = NormalizePath(app.tools.cmake);
    app.tools.ninja = NormalizePath(app.tools.ninja);
    app.tools.starmClang = NormalizePath(app.tools.starmClang);
    app.tools.gdb = NormalizePath(app.tools.gdb);
    app.tools.openocd = NormalizePath(app.tools.openocd);
    app.tools.openocdInterface = Trim(app.tools.openocdInterface);
    app.tools.openocdTarget = Trim(app.tools.openocdTarget);
    app.svd = NormalizePath(app.svd);
    app.tools.svd = app.svd;
    PushControls(app);
    const ValidationResult validation = ValidateTools(app.tools, false);
    if (!validation.ok)
    {
        MessageBoxW(owner, (L"以下路径无效：\r\n" + validation.message).c_str(), L"配置检查失败", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }
    if (!app.svd.empty() && !FileExists(app.svd))
    {
        MessageBoxW(owner, L"SVD 路径不为空但文件不存在，请重新选择。", L"配置检查失败", MB_OK | MB_ICONWARNING | MB_TOPMOST);
        return false;
    }
    const bool updateUserDefaults = ShouldUpdateUserDefaults(app, owner);
    std::vector<std::wstring> backupCandidates = ConfigurationFiles(app.workspace);
    if (updateUserDefaults) backupCandidates.push_back(GetUserDefaultSettingsPath());
    const BackupChoice backup = ConfirmBackups(owner, backupCandidates);
    if (backup == BackupChoice::Cancel)
    {
        SetStatus(app, L"已取消工具链配置写入。");
        return false;
    }

    std::wstring error;
    const bool createBackup = backup == BackupChoice::Create;
    if (!WriteConfiguration(app.workspace, app.tools, app.projectName, app.chipType, app.svd, fromExample, error, createBackup))
    {
        MessageBoxW(owner, error.c_str(), L"写入配置失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
        return false;
    }
    if (updateUserDefaults && !WriteUserDefaultSettings(SharedTools(app.tools), error, createBackup))
        MessageBoxW(owner, error.c_str(), L"更新默认配置失败", MB_OK | MB_ICONWARNING | MB_TOPMOST);
    SetStatus(app, app.workspace.hasExample ? (fromExample ? L"工具链配置已重建。" : L"工具链配置已修改。") : L"工具链配置已生成。");
    return true;
}

} // namespace pathconfig::toolchain
