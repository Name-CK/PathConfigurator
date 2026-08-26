#include "core.hpp"

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <cwctype>

using namespace pathconfig;

namespace
{
	// 统一逻辑尺寸，避免不同 DPI 下控件发生相互挤压。
	constexpr int Ui(int value)
	{
		return (value * 22 + 12) / 25;
	}

	enum : int
	{
		ID_CMAKE = 100,
		ID_NINJA,
		ID_CLANG,
		ID_GDB,
		ID_OPENOCD,
		ID_OPENOCD_INTERFACE,
		ID_OPENOCD_TARGET,
		ID_SVD,
		ID_BROWSE_BASE = 200,
		ID_SAVE_EXAMPLE = 300,
		ID_SAVE_SETTINGS = 301,
		ID_HELP = 302,
		ID_CONFIG_SAVE_DEFAULT = 303,
		ID_CONFIG_LOAD_DEFAULT = 304,
		ID_CONFIG_CHECK_CLT = 305,
		ID_CLEAR = 306,
		ID_PAGE_TAB = 320,
		ID_TARGET_CATEGORY = 321,
		ID_TARGET_ADD_FILE = 322,
		ID_TARGET_ADD_FOLDER = 323,
		ID_TARGET_DELETE = 324,
		ID_TARGET_SAVE = 325
	};

	struct AppState
	{
		WorkspaceInfo workspace;
		ToolPaths tools;
		std::wstring projectName;
		std::wstring chipType;
		std::wstring svd;
		CMakeTargetConfig cmakeTarget;
		HWND edits[8]{};
		HWND openocdConfigBrowse[2]{};
		HWND pageTab = nullptr;
		HWND targetCategory = nullptr;
		HWND targetList = nullptr;
		HWND targetAddFile = nullptr;
		HWND targetAddFolder = nullptr;
		HWND targetDelete = nullptr;
		HWND targetSave = nullptr;
		std::vector<HWND> toolPageControls;
		std::vector<HWND> targetPageControls;
		HWND status = nullptr;
		HWND window = nullptr;
		HFONT font = nullptr;
		HFONT titleFont = nullptr;
		HMENU menu = nullptr;
	};

	AppState g_app;

	const wchar_t *Labels[] = {L"CMake", L"Ninja", L"starm-clang", L"ARM GDB", L"OpenOCD", L"调试器", L"目标配置文件", L"SVD"};
	const wchar_t *FileNames[] = {L"cmake.exe", L"ninja.exe", L"starm-clang.exe", L"arm-none-eabi-gdb.exe", L"openocd.exe", L"", L"", L".svd"};
	const bool HasBrowse[] = {true, true, true, true, true, true, true, true};

	enum TargetCategory : int { TargetSources, TargetIncludes, TargetDefinitions, TargetLinkDirectories };

	void AddToolPageControl(HWND control)
	{
		if (control) g_app.toolPageControls.push_back(control);
	}

	void AddTargetPageControl(HWND control)
	{
		if (control) g_app.targetPageControls.push_back(control);
	}

	std::wstring GetEdit(HWND h)
	{
		int n = GetWindowTextLengthW(h);
		std::wstring value(static_cast<size_t>(n) + 1, L'\0');
		if (n > 0)
			GetWindowTextW(h, value.data(), n + 1);
		value.resize(static_cast<size_t>(n));
		return Trim(value);
	}

	void SetEdit(HWND h, const std::wstring &value)
	{
		if (!h)
			return;
		SetWindowTextW(h, value.c_str());
	}

	int SelectedTargetCategory()
	{
		if (!g_app.targetCategory) return TargetSources;
		const LRESULT selected = SendMessageW(g_app.targetCategory, CB_GETCURSEL, 0, 0);
		return selected >= TargetSources && selected <= TargetLinkDirectories ? static_cast<int>(selected) : TargetSources;
	}

	bool SameConfigValue(const std::wstring& left, const std::wstring& right)
	{
		return _wcsicmp(Trim(left).c_str(), Trim(right).c_str()) == 0;
	}

	std::wstring ProjectRelativeDisplayPath(const std::wstring& cmakeRelativePath, bool isDirectory)
	{
		const std::wstring absolute = NormalizePath(JoinPath(g_app.workspace.toolchainRoot, cmakeRelativePath));
		wchar_t relative[MAX_PATH * 4]{};
		const DWORD attributes = isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
		if (!absolute.empty() && PathRelativePathToW(relative, g_app.workspace.root.c_str(), FILE_ATTRIBUTE_DIRECTORY,
			absolute.c_str(), attributes))
		{
			std::wstring value = relative;
			while (value.rfind(L".\\", 0) == 0) value.erase(0, 2);
			for (wchar_t& c : value) if (c == L'/') c = L'\\';
			if (value.empty() || value == L".") return L"\\";
			return value.front() == L'\\' ? value : L"\\" + value;
		}
		std::wstring value = cmakeRelativePath;
		for (wchar_t& c : value) if (c == L'/') c = L'\\';
		return value.empty() || value.front() == L'\\' ? value : L"\\" + value;
	}

	void RefreshTargetList()
	{
		if (!g_app.targetList) return;
		ListView_DeleteAllItems(g_app.targetList);
		const int category = SelectedTargetCategory();
		auto add = [](int row, const std::wstring& value) {
			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = row;
			item.pszText = const_cast<wchar_t*>(value.c_str());
			ListView_InsertItem(g_app.targetList, &item);
		};
		if (category == TargetSources)
		{
			for (size_t i = 0; i < g_app.cmakeTarget.sources.size(); ++i)
				add(static_cast<int>(i), ProjectRelativeDisplayPath(g_app.cmakeTarget.sources[i].path, g_app.cmakeTarget.sources[i].isFolder));
		}
		else
		{
			const std::vector<std::wstring>* values = category == TargetIncludes ? &g_app.cmakeTarget.includeDirectories :
				(category == TargetDefinitions ? &g_app.cmakeTarget.compileDefinitions : &g_app.cmakeTarget.linkDirectories);
			const bool isPath = category != TargetDefinitions;
			for (size_t i = 0; i < values->size(); ++i)
				add(static_cast<int>(i), isPath ? ProjectRelativeDisplayPath((*values)[i], true) : (*values)[i]);
		}
		// 最后一行始终作为新项输入行，双击即可使用 ListView 原位编辑器创建配置。
		add(ListView_GetItemCount(g_app.targetList), L"");
	}

	void UpdateTargetCategoryControls()
	{
		const int category = SelectedTargetCategory();
		const bool sources = category == TargetSources;
		const bool folders = category == TargetIncludes || category == TargetLinkDirectories;
		// 操作按钮保持固定位置；仅切换可用状态，避免 ShowWindow/MoveWindow 留下旧绘制区域。
		if (g_app.targetAddFile) EnableWindow(g_app.targetAddFile, sources);
		if (g_app.targetAddFolder) EnableWindow(g_app.targetAddFolder, sources || folders);
		RefreshTargetList();
		RedrawWindow(g_app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}

	void SwitchPage()
	{
		const bool targetPage = g_app.pageTab && TabCtrl_GetCurSel(g_app.pageTab) == 1;
		for (HWND control : g_app.toolPageControls) ShowWindow(control, targetPage ? SW_HIDE : SW_SHOW);
		for (HWND control : g_app.targetPageControls) ShowWindow(control, targetPage ? SW_SHOW : SW_HIDE);
		if (targetPage) UpdateTargetCategoryControls();
	}

	std::wstring InterfaceNameFromControl(HWND control)
	{
		std::wstring value = GetEdit(control);
		constexpr wchar_t extension[] = L".cfg";
		const size_t extensionLength = ARRAY_SIZE(extension) - 1;
		if (value.size() > extensionLength && _wcsicmp(value.c_str() + value.size() - extensionLength, extension) == 0)
			value.resize(value.size() - extensionLength);
		return value;
	}

	void PopulateOpenOcdInterfaceCombo()
	{
		HWND combo = g_app.edits[5];
		if (!combo)
			return;

		std::wstring desired = InterfaceNameFromControl(combo);
		if (desired.empty())
			desired = g_app.tools.openocdInterface;
		SendMessageW(combo, CB_RESETCONTENT, 0, 0);

		std::wstring scripts;
		const bool hasScripts = g_app.edits[4] && DetectOpenOcdScripts(GetEdit(g_app.edits[4]), scripts);
		if (hasScripts)
		{
			// 仅提供推荐的、未废弃的 DAP 和 ST-LINK 配置，且必须实际存在。
			const wchar_t *names[] = {L"cmsis-dap", L"stlink"};
			for (const wchar_t *name : names)
			{
				if (FileExists(JoinPath(scripts, std::wstring(L"interface\\") + name + L".cfg")))
					SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((std::wstring(name) + L".cfg").c_str()));
			}
			// 文件选择器仍允许使用其它 interface 配置；只在它实际存在时保留该项。
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
		if (selected != CB_ERR)
			SendMessageW(combo, CB_SETCURSEL, selected, 0);
		EnableWindow(combo, hasScripts && selected != CB_ERR);
	}

	void UpdateOpenOcdConfigControls()
	{
		const bool enabled = g_app.edits[4] && FileExists(GetEdit(g_app.edits[4]));
		PopulateOpenOcdInterfaceCombo();
		if (g_app.edits[6]) EnableWindow(g_app.edits[6], enabled);
		for (HWND button : g_app.openocdConfigBrowse)
			if (button) EnableWindow(button, enabled);
	}

	void ApplyModernTheme(HWND control, HFONT font)
	{
		if (!control)
			return;
		SetWindowTheme(control, L"Explorer", nullptr);
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	void PullControls()
	{
		g_app.tools.cmake = GetEdit(g_app.edits[0]);
		g_app.tools.ninja = GetEdit(g_app.edits[1]);
		g_app.tools.starmClang = GetEdit(g_app.edits[2]);
		g_app.tools.gdb = GetEdit(g_app.edits[3]);
		g_app.tools.openocd = GetEdit(g_app.edits[4]);
		g_app.tools.openocdInterface = InterfaceNameFromControl(g_app.edits[5]);
		g_app.tools.openocdTarget = GetEdit(g_app.edits[6]);
		g_app.svd = GetEdit(g_app.edits[7]);
		g_app.tools.svd = g_app.svd;
	}

	void PushControls()
	{
		SetEdit(g_app.edits[0], g_app.tools.cmake);
		SetEdit(g_app.edits[1], g_app.tools.ninja);
		SetEdit(g_app.edits[2], g_app.tools.starmClang);
		SetEdit(g_app.edits[3], g_app.tools.gdb);
		SetEdit(g_app.edits[4], g_app.tools.openocd);
		SetEdit(g_app.edits[6], g_app.tools.openocdTarget);
		SetEdit(g_app.edits[7], g_app.svd);
		if (g_app.window)
		{
			SetDlgItemTextW(g_app.window, ID_CMAKE, g_app.tools.cmake.c_str());
			SetDlgItemTextW(g_app.window, ID_NINJA, g_app.tools.ninja.c_str());
			SetDlgItemTextW(g_app.window, ID_CLANG, g_app.tools.starmClang.c_str());
			SetDlgItemTextW(g_app.window, ID_GDB, g_app.tools.gdb.c_str());
			SetDlgItemTextW(g_app.window, ID_OPENOCD, g_app.tools.openocd.c_str());
			SetDlgItemTextW(g_app.window, ID_OPENOCD_TARGET, g_app.tools.openocdTarget.c_str());
			SetDlgItemTextW(g_app.window, ID_SVD, g_app.svd.c_str());
			UpdateOpenOcdConfigControls();
		}
		g_app.tools.svd = g_app.svd;
	}

	const std::wstring &InitialValue(int index)
	{
		switch (index)
		{
		case 0:
			return g_app.tools.cmake;
		case 1:
			return g_app.tools.ninja;
		case 2:
			return g_app.tools.starmClang;
		case 3:
			return g_app.tools.gdb;
		case 4:
			return g_app.tools.openocd;
		case 5:
			return g_app.tools.openocdInterface;
		case 6:
			return g_app.tools.openocdTarget;
		case 7:
			return g_app.svd;
		default:
			return g_app.svd;
		}
	}

	void Status(const std::wstring &text)
	{
		if (g_app.status)
			SetWindowTextW(g_app.status, text.c_str());
	}

	ToolPaths SharedTools(const ToolPaths &tools)
	{
		ToolPaths shared = tools;
		shared.svd.clear();
		shared.openocdTarget.clear();
		return shared;
	}

	bool SamePath(const std::wstring &left, const std::wstring &right)
	{
		return _wcsicmp(NormalizePath(left).c_str(), NormalizePath(right).c_str()) == 0;
	}

	bool SameSharedTools(const ToolPaths &left, const ToolPaths &right)
	{
		return SamePath(left.cmake, right.cmake) && SamePath(left.ninja, right.ninja) &&
			SamePath(left.starmClang, right.starmClang) && SamePath(left.gdb, right.gdb) &&
			SamePath(left.openocd, right.openocd) &&
			_wcsicmp(Trim(left.openocdInterface).c_str(), Trim(right.openocdInterface).c_str()) == 0;
	}

	bool HasAnySharedTool(const ToolPaths &tools)
	{
		return !tools.cmake.empty() || !tools.ninja.empty() || !tools.starmClang.empty() ||
			!tools.gdb.empty() || !tools.openocd.empty() || !tools.openocdInterface.empty();
	}

	bool HasCompleteSharedToolchain(const ToolPaths &tools)
	{
		return !tools.cmake.empty() && !tools.ninja.empty() && !tools.starmClang.empty() &&
			!tools.gdb.empty() && !tools.openocd.empty();
	}

	void MergeSharedTools(ToolPaths &destination, const ToolPaths &source, bool overwrite)
	{
		std::wstring *destinationValues[] = {
			&destination.cmake, &destination.ninja, &destination.starmClang, &destination.gdb,
			&destination.openocd, &destination.openocdInterface};
		const std::wstring *sourceValues[] = {
			&source.cmake, &source.ninja, &source.starmClang, &source.gdb,
			&source.openocd, &source.openocdInterface};
		for (size_t i = 0; i < ARRAY_SIZE(destinationValues); ++i) {
			if (!sourceValues[i]->empty() && (overwrite || destinationValues[i]->empty()))
				*destinationValues[i] = *sourceValues[i];
		}
	}

	std::wstring DescribeSharedTools(const ToolPaths &tools)
	{
		struct Entry { const wchar_t *name; const std::wstring *path; } entries[] = {
			{L"CMake", &tools.cmake}, {L"Ninja", &tools.ninja}, {L"starm-clang", &tools.starmClang},
			{L"ARM GDB", &tools.gdb}, {L"OpenOCD", &tools.openocd}, {L"调试器", &tools.openocdInterface}};
		std::wstring text;
		for (const Entry &entry : entries) {
			if (!entry.path->empty()) text += std::wstring(entry.name) + L"：" + *entry.path + L"\r\n";
		}
		return text.empty() ? L"（没有有效的默认工具路径）\r\n" : text;
	}

	void SanitizeCurrentTools()
	{
		PullControls();
		SanitizeToolPaths(g_app.tools);
		g_app.svd = g_app.tools.svd;
	}

	void ShowFileHelp(HWND owner)
	{
		const std::wstring root = g_app.workspace.root;
		const std::wstring toolchain = g_app.workspace.toolchainRoot;
		const std::wstring defaults = GetUserDefaultSettingsPath();
		std::wstring text =
			L"STM32 项目配置器 - 文件读写说明\r\n\r\n"
			L"程序使用唯一的自定义配置前缀：CustomCfg.*\r\n\r\n"
			L"启动和检查时读取：\r\n"
			L"1. " + root + L"\\*.ioc\r\n"
			L"   检查 TargetToolchain、ToolChainLocation，并读取芯片类型。\r\n"
			L"2. " + toolchain + L"\\CMakeLists.txt\r\n"
			L"   读取 CMake 项目名称，并确认 CMake 代码已经生成。\r\n"
			L"3. " + toolchain + L"\\project-config.json（如果存在）\r\n"
			L"   读取可视化的 CMake 目标配置。\r\n"
			L"4. " + root + L"\\.vscode\\settings.json（如果存在）\r\n"
			L"   预填工具路径和项目配置。\r\n\r\n"
			L"点击“确认并生成（从 example）”时还会读取：\r\n"
			L"5. " + root + L"\\.vscode\\settings.example.json\r\n"
			L"   作为必要配置模板，并保留模板中的其它字段。\r\n\r\n"
			L"点击“确认并生成（从 settings）”时读取：\r\n"
			L"5. " + root + L"\\.vscode\\settings.json\r\n"
			L"   在现有配置上更新工具链字段，保留个人配置和未知字段。\r\n\r\n"
			L"程序会修改或生成：\r\n"
			L"1. " + root + L"\\.vscode\\settings.json\r\n"
			L"   写入 CustomCfg.*、CMake Tools、C/C++ 和调试所需路径。\r\n"
			L"2. " + toolchain + L"\\CMakeUserPresets.json\r\n"
			L"   写入本机 Ninja 和编译器路径的 CMake 预设。\r\n"
			L"3. " + toolchain + L"\\project-config.json\r\n"
			L"   在“CMake 目标配置”页保存时写入，适合提交 Git。\r\n"
			L"4. " + toolchain + L"\\cmake\\PathConfiguratorProject.cmake\r\n"
			L"   在“CMake 目标配置”页保存时生成，适合提交 Git。\r\n"
			L"5. " + toolchain + L"\\CMakeLists.txt\r\n"
			L"   首次保存 CMake 目标配置时补入 PathConfigurator 的 include 行。\r\n"
			L"6. " + defaults + L"\r\n"
			L"   默认配置，仅保存共享工具路径和调试器，不保存 SVD 或 target。\r\n"
			L"7. 上述文件的 .bak 备份（存在旧文件时）。\r\n\r\n"
			L"程序不会修改 .ioc。";
		MessageBoxW(owner, text.c_str(), L"帮助 - 文件读写说明", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
	}

	void ApplyOpenOcdDefaults()
	{
		if (FileExists(g_app.tools.openocd) && g_app.tools.openocdInterface.empty()) {
			g_app.tools.openocdInterface = L"cmsis-dap";
			SanitizeToolPaths(g_app.tools);
		}
		if (g_app.tools.openocdTarget.empty())
			g_app.tools.openocdTarget = FindOpenOcdTargetForChip(g_app.tools.openocd, g_app.chipType);
	}

	bool TryAutoFillSvdFromCurrentCubeClt()
	{
		if (!g_app.svd.empty() && FileExists(g_app.svd))
			return false;
		const std::wstring *candidates[] = {
			&g_app.tools.cmake, &g_app.tools.ninja, &g_app.tools.starmClang, &g_app.tools.gdb};
		for (const std::wstring *candidate : candidates)
		{
			if (candidate->empty()) continue;
			ToolPaths detected;
			std::wstring cubeRoot, report;
			if (!DetectCubeClt(*candidate, detected, cubeRoot, report)) continue;
			const std::wstring svd = FindSvdForChip(cubeRoot, g_app.chipType);
			if (!svd.empty() && FileExists(svd)) {
				g_app.svd = svd;
				g_app.tools.svd = svd;
				return true;
			}
		}
		return false;
	}

	void ClearCurrentConfiguration(HWND owner)
	{
		if (MessageBoxW(owner, L"清空当前界面中的全部路径、调试和 CMake 目标配置？\r\n\r\n此操作不会修改或删除任何 settings.json、默认配置、CMakeUserPresets.json 或 project-config.json 文件。",
			L"清空配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES)
			return;
		g_app.tools = {};
		g_app.svd.clear();
		g_app.cmakeTarget = {};
		PushControls();
		RefreshTargetList();
		Status(L"已清空当前界面配置；尚未修改任何文件。");
	}

	void OfferRegistryClt(HWND owner, bool showMissing)
	{
		ToolPaths registryTools;
		std::wstring registryRoot, registryReport;
		if (!DetectCubeCltFromRegistry(registryTools, registryRoot, registryReport)) {
			if (showMissing)
				MessageBoxW(owner, L"未在注册表中找到有效的 STM32CubeCLT 安装记录。", L"检查 STM32CubeCLT", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
			return;
		}
		SanitizeToolPaths(registryTools);
		const std::wstring registrySvd = FindSvdForChip(registryRoot, g_app.chipType);
		if (!registrySvd.empty()) registryReport += L"SVD：" + registrySvd + L"\r\n";
		const std::wstring prompt = L"检测到注册表中的 STM32CubeCLT 工具：\r\n\r\n" + registryReport +
			L"\r\n是否补充到当前配置界面？\r\n已有的有效工具路径不会被覆盖。";
		if (MessageBoxW(owner, prompt.c_str(), L"检查 STM32CubeCLT", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES)
			return;
		MergeSharedTools(g_app.tools, registryTools, false);
		if (g_app.svd.empty() && !registrySvd.empty()) g_app.svd = registrySvd;
		g_app.tools.svd = g_app.svd;
		ApplyOpenOcdDefaults();
		PushControls();
		Status(L"已补充注册表检测到的 STM32CubeCLT 工具路径。");
	}

	void OfferUserDefaultsOnStartup(HWND owner, bool hasProjectSettings)
	{
		if (HasCompleteSharedToolchain(g_app.tools))
			return;
		ToolPaths defaults;
		std::wstring error;
		if (!LoadUserDefaultSettings(defaults, error) || !HasAnySharedTool(defaults)) {
			OfferRegistryClt(owner, false);
			return;
		}
		const std::wstring reason = hasProjectSettings
			? L"工程 .vscode/settings.json 中存在缺失或无效的工具路径。"
			: L"未检测到可用的工程 .vscode/settings.json。";
		const std::wstring prompt = reason + L"\r\n\r\n是否使用已有默认配置补充当前工程？\r\n\r\n" +
			DescribeSharedTools(defaults) + L"\r\n工程中已有的有效路径不会被覆盖。";
		if (MessageBoxW(owner, prompt.c_str(), L"读取默认配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES)
			return;
		MergeSharedTools(g_app.tools, defaults, false);
		ApplyOpenOcdDefaults();
		if (!HasCompleteSharedToolchain(g_app.tools)) {
			MessageBoxW(owner, L"默认配置未能补齐全部工具路径，将继续检查注册表中的 STM32CubeCLT。",
				L"默认配置不完整", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
			OfferRegistryClt(owner, false);
		}
	}

	void ReadUserDefaultsFromMenu(HWND owner)
	{
		PullControls();
		ToolPaths defaults;
		std::wstring error;
		if (!LoadUserDefaultSettings(defaults, error)) {
			MessageBoxW(owner, error.c_str(), L"读取默认配置", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
			return;
		}
		if (!HasAnySharedTool(defaults)) {
			MessageBoxW(owner, L"默认配置中没有有效的工具路径。", L"读取默认配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return;
		}
		const std::wstring prompt = L"是否读取下列默认配置？\r\n\r\n" + DescribeSharedTools(defaults) +
			L"\r\n这会替换界面中的共享工具路径；不会修改 SVD 或 target。";
		if (MessageBoxW(owner, prompt.c_str(), L"读取默认配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES)
			return;
		MergeSharedTools(g_app.tools, defaults, true);
		ApplyOpenOcdDefaults();
		const bool foundSvd = TryAutoFillSvdFromCurrentCubeClt();
		PushControls();
		Status(foundSvd ? L"已读取默认配置，并从 STM32CubeCLT 自动匹配当前芯片的 SVD 文件。"
			: L"已读取默认配置；未找到当前芯片的 SVD，保留现有 SVD 与目标配置文件。");
	}

	void SaveUserDefaultsFromMenu(HWND owner)
	{
		SanitizeCurrentTools();
		ToolPaths defaults = SharedTools(g_app.tools);
		ValidationResult validation = ValidateTools(defaults, false);
		if (!validation.ok) {
			MessageBoxW(owner, (L"默认配置需要完整且有效的共享工具路径：\r\n" + validation.message).c_str(),
				L"保存默认配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return;
		}
		std::wstring error;
		if (!WriteUserDefaultSettings(defaults, error)) {
			MessageBoxW(owner, error.c_str(), L"保存默认配置", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return;
		}
		MessageBoxW(owner, (L"默认配置已保存：\r\n" + GetUserDefaultSettingsPath() +
			L"\r\n\r\n不包含 SVD、target 或其它工程专属配置。").c_str(),
			L"保存默认配置", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
		Status(L"已保存本机默认工具配置。");
	}

	void OfferUpdateUserDefaults(HWND owner)
	{
		ToolPaths defaults = SharedTools(g_app.tools);
		ToolPaths cached;
		std::wstring error;
		const bool hasCache = LoadUserDefaultSettings(cached, error);
		if (hasCache && SameSharedTools(defaults, cached))
			return;
		const std::wstring prompt = L"当前工程的共享工具路径与默认配置不同。\r\n\r\n是否更新本机默认配置？\r\n\r\n" +
			DescribeSharedTools(defaults) + L"\r\n不会保存 SVD 或 target。";
		if (MessageBoxW(owner, prompt.c_str(), L"更新默认配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES)
			return;
		if (!WriteUserDefaultSettings(defaults, error)) {
			MessageBoxW(owner, error.c_str(), L"更新默认配置失败", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return;
		}
		Status(L"工程配置已生成，并已更新本机默认工具配置。");
	}

	std::wstring SelectFile(HWND owner, const wchar_t *expectedName)
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
		std::wstring title = std::wstring(L"选择 ") + expectedName;
		ofn.lpstrFilter = filter.c_str();
		ofn.lpstrTitle = title.c_str();
		ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
		if (GetOpenFileNameW(&ofn) == TRUE)
			return NormalizePath(path);
		return {};
	}

	std::wstring SelectTargetSourceFile(HWND owner)
	{
		wchar_t path[4096]{};
		const wchar_t filter[] =
			L"C/C++/汇编源文件 (*.c;*.cc;*.cp;*.cpp;*.cxx;*.s;*.S;*.asm)\0*.c;*.cc;*.cp;*.cpp;*.cxx;*.s;*.S;*.asm\0"
			L"所有文件\0*.*\0\0";
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = owner;
		ofn.lpstrFile = path;
		ofn.nMaxFile = static_cast<DWORD>(ARRAY_SIZE(path));
		ofn.lpstrFilter = filter;
		ofn.lpstrTitle = L"选择要加入 CMake 目标的源文件";
		ofn.lpstrInitialDir = g_app.workspace.root.c_str();
		ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
		return GetOpenFileNameW(&ofn) == TRUE ? NormalizePath(path) : std::wstring{};
	}

	std::wstring SelectFolder(HWND owner, const wchar_t* title)
	{
		IFileOpenDialog* dialog = nullptr;
		if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog))))
			return {};
		FILEOPENDIALOGOPTIONS options{};
		dialog->GetOptions(&options);
		dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR);
		dialog->SetTitle(title);
		IShellItem* initialItem = nullptr;
		if (SUCCEEDED(SHCreateItemFromParsingName(g_app.workspace.root.c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&initialItem)))) {
			dialog->SetFolder(initialItem);
			initialItem->Release();
		}
		std::wstring path;
		if (SUCCEEDED(dialog->Show(owner))) {
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item))) {
				PWSTR raw = nullptr;
				if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
					path = NormalizePath(raw);
					CoTaskMemFree(raw);
				}
				item->Release();
			}
		}
		dialog->Release();
		return path;
	}

	bool AddCmakePathValue(HWND owner, const std::wstring& selected, bool isFolder)
	{
		const int category = SelectedTargetCategory();
		std::wstring relative;
		if (!MakeToolchainRelativePath(g_app.workspace, selected, isFolder, relative)) {
			MessageBoxW(owner, L"所选路径无法转换为相对于 .ioc 指定 CMake 目录的路径。\r\n请选择与当前工程位于同一磁盘分区的文件或文件夹。",
				L"无法添加 CMake 项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return false;
		}
		if (category == TargetSources) {
			for (const TargetSourceEntry& entry : g_app.cmakeTarget.sources) {
				if (entry.isFolder == isFolder && SameConfigValue(entry.path, relative)) return false;
			}
			g_app.cmakeTarget.sources.push_back({relative, isFolder});
		} else {
			std::vector<std::wstring>* values = category == TargetIncludes ? &g_app.cmakeTarget.includeDirectories : &g_app.cmakeTarget.linkDirectories;
			for (const std::wstring& value : *values) if (SameConfigValue(value, relative)) return false;
			values->push_back(relative);
		}
		RefreshTargetList();
		Status(L"已加入 CMake 目标配置；点击“保存 CMake 目标配置”后写入工程文件。");
		return true;
	}

	bool AddCmakePath(HWND owner, bool isFolder)
	{
		const int category = SelectedTargetCategory();
		const std::wstring selected = isFolder
			? SelectFolder(owner, category == TargetSources ? L"选择递归加入的源文件夹" : L"选择要加入 CMake 的文件夹")
			: SelectTargetSourceFile(owner);
		return selected.empty() ? false : AddCmakePathValue(owner, selected, isFolder);
	}

	bool CommitTargetListEdit(HWND owner, int row, const std::wstring& input)
	{
		const std::wstring value = Trim(input);
		if (value.empty()) return false;
		const int category = SelectedTargetCategory();
		if (category == TargetDefinitions) {
			std::vector<std::wstring>& values = g_app.cmakeTarget.compileDefinitions;
			if (row < 0 || static_cast<size_t>(row) > values.size()) return false;
			for (size_t i = 0; i < values.size(); ++i)
				if (static_cast<int>(i) != row && SameConfigValue(values[i], value)) return false;
			if (static_cast<size_t>(row) == values.size()) values.push_back(value);
			else values[static_cast<size_t>(row)] = value;
		} else {
			std::wstring candidate = value;
			const bool rootRelative = !candidate.empty() && (candidate.front() == L'\\' || candidate.front() == L'/') &&
				!(candidate.size() > 1 && (candidate[1] == L'\\' || candidate[1] == L'/'));
			if (rootRelative) candidate.erase(0, 1);
			if (rootRelative || PathIsRelativeW(candidate.c_str())) candidate = JoinPath(g_app.workspace.root, candidate);
			candidate = NormalizePath(candidate);
			const bool isFile = FileExists(candidate);
			const bool isFolder = FolderExists(candidate);
			if (!isFile && !isFolder) {
				MessageBoxW(owner, (L"路径不存在：\r\n" + candidate).c_str(), L"无法编辑 CMake 项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			if (category != TargetSources && !isFolder) {
				MessageBoxW(owner, L"当前配置类别仅接受文件夹路径。", L"无法编辑 CMake 项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			std::wstring relative;
			if (!MakeToolchainRelativePath(g_app.workspace, candidate, isFolder, relative)) {
				MessageBoxW(owner, L"所选路径无法转换为相对于 .ioc 指定 CMake 目录的路径。", L"无法编辑 CMake 项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			if (category == TargetSources) {
				std::vector<TargetSourceEntry>& values = g_app.cmakeTarget.sources;
				if (row < 0 || static_cast<size_t>(row) > values.size()) return false;
				for (size_t i = 0; i < values.size(); ++i)
					if (static_cast<int>(i) != row && values[i].isFolder == isFolder && SameConfigValue(values[i].path, relative)) return false;
				if (static_cast<size_t>(row) == values.size()) values.push_back({relative, isFolder});
				else values[static_cast<size_t>(row)] = {relative, isFolder};
			} else {
				std::vector<std::wstring>& values = category == TargetIncludes ? g_app.cmakeTarget.includeDirectories : g_app.cmakeTarget.linkDirectories;
				if (row < 0 || static_cast<size_t>(row) > values.size()) return false;
				for (size_t i = 0; i < values.size(); ++i)
					if (static_cast<int>(i) != row && SameConfigValue(values[i], relative)) return false;
				if (static_cast<size_t>(row) == values.size()) values.push_back(relative);
				else values[static_cast<size_t>(row)] = relative;
			}
		}
		RefreshTargetList();
		Status(L"已更新 CMake 目标配置；点击“保存 CMake 目标配置”后写入工程文件。");
		return true;
	}

	void DeleteSelectedCmakeTargetEntry()
	{
		const int index = ListView_GetNextItem(g_app.targetList, -1, LVNI_SELECTED);
		if (index < 0) return;
		const int category = SelectedTargetCategory();
		if (category == TargetSources) {
			if (static_cast<size_t>(index) < g_app.cmakeTarget.sources.size())
				g_app.cmakeTarget.sources.erase(g_app.cmakeTarget.sources.begin() + index);
		} else {
			std::vector<std::wstring>* values = category == TargetIncludes ? &g_app.cmakeTarget.includeDirectories :
				(category == TargetDefinitions ? &g_app.cmakeTarget.compileDefinitions : &g_app.cmakeTarget.linkDirectories);
			if (static_cast<size_t>(index) < values->size()) values->erase(values->begin() + index);
		}
		RefreshTargetList();
	}

	bool SaveCmakeTargetConfig(HWND owner)
	{
		std::wstring error;
		if (!WriteCMakeTargetConfig(g_app.workspace, g_app.cmakeTarget, error)) {
			MessageBoxW(owner, error.c_str(), L"保存 CMake 目标配置失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return false;
		}
		const std::wstring text = L"CMake 目标配置已保存。\r\n\r\n已更新：\r\n" +
			g_app.workspace.cmakeTargetConfigPath + L"\r\n" + g_app.workspace.cmakeTargetModulePath +
			L"\r\n" + JoinPath(g_app.workspace.toolchainRoot, L"CMakeLists.txt") +
			L"\r\n\r\n目录项会递归收集 C/C++/汇编源文件；请重新 Configure CMake。";
		MessageBoxW(owner, text.c_str(), L"CMake 目标配置已保存", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
		Status(L"CMake 目标配置已保存，请在 CMake Tools 执行 Configure。 ");
		return true;
	}

	std::wstring SelectOpenOcdConfig(HWND owner, const std::wstring &openocdPath, const wchar_t *folder, const wchar_t *title)
	{
		std::wstring scripts;
		if (!DetectOpenOcdScripts(openocdPath, scripts))
		{
			MessageBoxW(owner, L"无法从当前 openocd.exe 的安装目录找到 scripts 文件夹。\r\n请先选择有效的 OpenOCD 程序。",
				L"无法选择 OpenOCD 配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return {};
		}
		std::wstring configFolder = JoinPath(scripts, folder);
		if (!FolderExists(configFolder))
		{
			MessageBoxW(owner, (L"未找到 OpenOCD 配置目录：\r\n" + configFolder).c_str(),
				L"无法选择 OpenOCD 配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return {};
		}
		wchar_t path[4096]{};
		const wchar_t filter[] = L"OpenOCD 配置文件 (*.cfg)\0*.cfg\0所有文件\0*.*\0\0";
		OPENFILENAMEW ofn{};
		ofn.lStructSize = sizeof(ofn);
		ofn.hwndOwner = owner;
		ofn.lpstrFile = path;
		ofn.nMaxFile = static_cast<DWORD>(ARRAY_SIZE(path));
		ofn.lpstrInitialDir = configFolder.c_str();
		ofn.lpstrFilter = filter;
		ofn.lpstrTitle = title;
		ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
		if (GetOpenFileNameW(&ofn) != TRUE)
			return {};
		return NormalizePath(path);
	}

	void TryCubeClt(HWND owner, int index)
	{
		if (index > 2)
			return;
		PullControls();
		ToolPaths detected = g_app.tools;
		std::wstring root, report;
		if (!DetectCubeClt(GetEdit(g_app.edits[index]), detected, root, report))
			return;
		std::wstring svd = FindSvdForChip(root, g_app.chipType);
		if (!svd.empty())
			report += L"SVD = " + svd + L"\r\n";
		std::wstring message = L"检测到 STM32CubeCLT 工具包：\r\n" + root + L"\r\n\r\n" + report + L"\r\n是否自动填充检测到的地址？";
		if (MessageBoxW(owner, message.c_str(), L"检测 STM32CubeCLT", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES)
		{
			g_app.tools = detected;
			if (!svd.empty())
				g_app.svd = svd;
			PushControls();
			Status(L"已从 STM32CubeCLT 自动填充工具链地址。");
		}
	}

	void Browse(HWND owner, int index)
	{
		if (!HasBrowse[index])
			return;
		if (index == 5 || index == 6)
		{
			PullControls();
			const bool isInterfaceConfig = index == 5;
			std::wstring selected = SelectOpenOcdConfig(owner, g_app.tools.openocd, isInterfaceConfig ? L"interface" : L"target",
				isInterfaceConfig ? L"选择 OpenOCD interface 配置" : L"选择 OpenOCD target 配置");
			if (selected.empty()) return;
			std::wstring value = FileName(selected);
			if (isInterfaceConfig)
			{
				if (value.size() > 4 && _wcsicmp(value.c_str() + value.size() - 4, L".cfg") == 0)
					value.resize(value.size() - 4);
				g_app.tools.openocdInterface = value;
				PopulateOpenOcdInterfaceCombo();
			}
			else
				SetEdit(g_app.edits[index], value);
			return;
		}
		std::wstring selected = SelectFile(owner, FileNames[index]);
		if (selected.empty())
			return;
		SetEdit(g_app.edits[index], selected);
		if (index == 4 && GetEdit(g_app.edits[6]).empty())
		{
			std::wstring target = FindOpenOcdTargetForChip(selected, g_app.chipType);
			if (!target.empty())
			{
				SetEdit(g_app.edits[6], target);
				Status(L"已根据芯片型号从 OpenOCD target 配置中选择默认文件。");
			}
		}
		if (index == 4)
			UpdateOpenOcdConfigControls();
		if (index <= 2)
		{
			TryCubeClt(owner, index);
		}
	}

	bool Save(HWND owner, bool fromExample)
	{
		PullControls();
		g_app.tools.cmake = NormalizePath(g_app.tools.cmake);
		g_app.tools.ninja = NormalizePath(g_app.tools.ninja);
		g_app.tools.starmClang = NormalizePath(g_app.tools.starmClang);
		g_app.tools.gdb = NormalizePath(g_app.tools.gdb);
		g_app.tools.openocd = NormalizePath(g_app.tools.openocd);
		g_app.tools.openocdInterface = Trim(g_app.tools.openocdInterface);
		g_app.tools.openocdTarget = Trim(g_app.tools.openocdTarget);
		g_app.svd = NormalizePath(g_app.svd);
		g_app.tools.svd = g_app.svd;
		PushControls();
		ValidationResult validation = ValidateTools(g_app.tools, false);
		if (!validation.ok)
		{
			MessageBoxW(owner, (L"以下路径无效：\r\n" + validation.message).c_str(), L"配置检查失败", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return false;
		}
		if (!g_app.svd.empty() && !FileExists(g_app.svd))
		{
			MessageBoxW(owner, L"SVD 路径不为空但文件不存在，请重新选择。", L"配置检查失败", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return false;
		}
		std::wstring error;
		if (!WriteConfiguration(g_app.workspace, g_app.tools, g_app.projectName, g_app.chipType, g_app.svd, fromExample, error))
		{
			MessageBoxW(owner, error.c_str(), L"写入配置失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return false;
		}
		OfferUpdateUserDefaults(owner);
		MessageBoxW(owner, (L"本地工具链配置已生成。\r\n\r\n工程：" + g_app.workspace.projectLabel + L"\r\n项目名：" + g_app.projectName + L"\r\n芯片：" + g_app.chipType + L"\r\n\r\n已生成：\r\n" + g_app.workspace.settingsPath + L"\r\n" + g_app.workspace.presetsPath + L"\r\n请在 VSCode 执行 Reload Window。").c_str(), L"配置完成", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SYSTEMMODAL);
		Status(L"配置已写入，退出码为 0。");
		return true;
	}

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_CREATE:
		{
			g_app.window = hwnd;
			// 灰度抗锯齿避免截图中的 ClearType 彩边；微软雅黑 UI 更适合 Win32 控件的小字号显示。
			g_app.font = CreateFontW(-Ui(15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
									OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
			g_app.titleFont = CreateFontW(-Ui(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
																						OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
			g_app.menu = CreateMenu();
			HMENU configMenu = CreatePopupMenu();
			AppendMenuW(configMenu, MF_STRING, ID_CONFIG_SAVE_DEFAULT, L"保存为默认配置");
			AppendMenuW(configMenu, MF_STRING, ID_CONFIG_LOAD_DEFAULT, L"读取默认配置");
			AppendMenuW(configMenu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(configMenu, MF_STRING, ID_CONFIG_CHECK_CLT, L"从注册表检查 CLT 包");
			AppendMenuW(g_app.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(configMenu), L"配置(&C)");
			HMENU helpMenu = CreatePopupMenu();
			AppendMenuW(helpMenu, MF_STRING, ID_HELP, L"文件读写说明");
			AppendMenuW(g_app.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(helpMenu), L"帮助(&H)");
			SetMenu(hwnd, g_app.menu);
			HWND title = CreateWindowW(L"STATIC", (g_app.workspace.projectLabel + L"  STM32 项目配置").c_str(), WS_CHILD | WS_VISIBLE,
				Ui(26), Ui(18), Ui(840), Ui(30), hwnd, nullptr, nullptr, nullptr);
			HWND subtitle = CreateWindowW(L"STATIC", (L"项目：" + g_app.projectName + L"    芯片：" + g_app.chipType).c_str(),
				WS_CHILD | WS_VISIBLE, Ui(27), Ui(48), Ui(840), Ui(22), hwnd, nullptr, nullptr, nullptr);
			SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(g_app.titleFont), TRUE);
			ApplyModernTheme(subtitle, g_app.font);

			g_app.pageTab = CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
				Ui(20), Ui(82), Ui(870), Ui(500), hwnd, reinterpret_cast<HMENU>(ID_PAGE_TAB), nullptr, nullptr);
			TCITEMW toolTab{};
			toolTab.mask = TCIF_TEXT;
			toolTab.pszText = const_cast<wchar_t*>(L"工具链配置");
			TabCtrl_InsertItem(g_app.pageTab, 0, &toolTab);
			TCITEMW targetTab{};
			targetTab.mask = TCIF_TEXT;
			targetTab.pszText = const_cast<wchar_t*>(L"CMake 目标配置");
			TabCtrl_InsertItem(g_app.pageTab, 1, &targetTab);
			ApplyModernTheme(g_app.pageTab, g_app.font);

			int y = Ui(125);
			for (int i = 0; i < 4; ++i)
			{
				HWND label = CreateWindowW(L"STATIC", Labels[i], WS_CHILD | WS_VISIBLE, Ui(27), y + Ui(5), Ui(136), Ui(22), hwnd, nullptr, nullptr, nullptr);
				g_app.edits[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(i).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
					Ui(171), y, Ui(600), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_CMAKE + i), nullptr, nullptr);
				HWND browse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
					Ui(784), y, Ui(85), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + i), nullptr, nullptr);
				ApplyModernTheme(label, g_app.font); ApplyModernTheme(g_app.edits[i], g_app.font); ApplyModernTheme(browse, g_app.font);
				AddToolPageControl(label); AddToolPageControl(g_app.edits[i]); AddToolPageControl(browse);
				y += Ui(45);
			}
			HWND svdLabel = CreateWindowW(L"STATIC", Labels[7], WS_CHILD | WS_VISIBLE, Ui(27), y + Ui(5), Ui(136), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[7] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(7).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				Ui(171), y, Ui(600), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_SVD), nullptr, nullptr);
			HWND svdBrowse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(784), y, Ui(85), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 7), nullptr, nullptr);
			ApplyModernTheme(svdLabel, g_app.font); ApplyModernTheme(g_app.edits[7], g_app.font); ApplyModernTheme(svdBrowse, g_app.font);
			AddToolPageControl(svdLabel); AddToolPageControl(g_app.edits[7]); AddToolPageControl(svdBrowse);
			y += Ui(45);
			HWND ocdLabel = CreateWindowW(L"STATIC", Labels[4], WS_CHILD | WS_VISIBLE, Ui(27), y + Ui(5), Ui(136), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[4] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(4).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				Ui(171), y, Ui(600), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_OPENOCD), nullptr, nullptr);
			HWND ocdBrowse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(784), y, Ui(85), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 4), nullptr, nullptr);
			ApplyModernTheme(ocdLabel, g_app.font); ApplyModernTheme(g_app.edits[4], g_app.font); ApplyModernTheme(ocdBrowse, g_app.font);
			AddToolPageControl(ocdLabel); AddToolPageControl(g_app.edits[4]); AddToolPageControl(ocdBrowse);
			y += Ui(45);
			HWND interfaceLabel = CreateWindowW(L"STATIC", Labels[5], WS_CHILD | WS_VISIBLE, Ui(27), y + Ui(5), Ui(76), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[5] = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
				Ui(110), y, Ui(230), Ui(150), hwnd, reinterpret_cast<HMENU>(ID_OPENOCD_INTERFACE), nullptr, nullptr);
			g_app.openocdConfigBrowse[0] = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(350), y, Ui(85), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 5), nullptr, nullptr);
			HWND targetLabel = CreateWindowW(L"STATIC", Labels[6], WS_CHILD | WS_VISIBLE, Ui(450), y + Ui(5), Ui(116), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[6] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(6).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				Ui(575), y, Ui(200), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_OPENOCD_TARGET), nullptr, nullptr);
			g_app.openocdConfigBrowse[1] = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(784), y, Ui(85), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 6), nullptr, nullptr);
			ApplyModernTheme(interfaceLabel, g_app.font); ApplyModernTheme(g_app.edits[5], g_app.font); ApplyModernTheme(g_app.openocdConfigBrowse[0], g_app.font);
			ApplyModernTheme(targetLabel, g_app.font); ApplyModernTheme(g_app.edits[6], g_app.font); ApplyModernTheme(g_app.openocdConfigBrowse[1], g_app.font);
			AddToolPageControl(interfaceLabel); AddToolPageControl(g_app.edits[5]); AddToolPageControl(g_app.openocdConfigBrowse[0]);
			AddToolPageControl(targetLabel); AddToolPageControl(g_app.edits[6]); AddToolPageControl(g_app.openocdConfigBrowse[1]);
			HWND clear = CreateWindowW(L"BUTTON", L"清空", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(27), Ui(510), Ui(115), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_CLEAR), nullptr, nullptr);
			HWND ex = CreateWindowW(L"BUTTON", g_app.workspace.hasExample ? L"确认并生成（从 example）" : L"确认并生成", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				Ui(488), Ui(510), Ui(g_app.workspace.hasExample ? 190 : 150), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_SAVE_EXAMPLE), nullptr, nullptr);
			HWND settings = CreateWindowW(L"BUTTON", g_app.workspace.hasExample ? L"确认并生成（从 settings）" : L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(g_app.workspace.hasExample ? 690 : 650), Ui(510), Ui(g_app.workspace.hasExample ? 190 : 150), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_SAVE_SETTINGS), nullptr, nullptr);
			ApplyModernTheme(clear, g_app.font); ApplyModernTheme(ex, g_app.font); ApplyModernTheme(settings, g_app.font);
			AddToolPageControl(clear); AddToolPageControl(ex); AddToolPageControl(settings);

			HWND categoryLabel = CreateWindowW(L"STATIC", L"配置类别", WS_CHILD | WS_VISIBLE, Ui(27), Ui(126), Ui(100), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.targetCategory = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
				Ui(130), Ui(121), Ui(250), Ui(160), hwnd, reinterpret_cast<HMENU>(ID_TARGET_CATEGORY), nullptr, nullptr);
			const wchar_t* categories[] = {L"目标文件 / 文件夹", L"头文件目录", L"编译宏", L"链接目录"};
			for (const wchar_t* category : categories) SendMessageW(g_app.targetCategory, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(category));
			SendMessageW(g_app.targetCategory, CB_SETCURSEL, TargetSources, 0);
			g_app.targetList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
				WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
				Ui(27), Ui(167), Ui(842), Ui(310), hwnd, nullptr, nullptr, nullptr);
			LVCOLUMNW valueColumn{};
			valueColumn.mask = LVCF_TEXT | LVCF_WIDTH;
			valueColumn.pszText = const_cast<wchar_t*>(L"项目根目录相对路径 / 宏");
			valueColumn.cx = Ui(838);
			ListView_InsertColumn(g_app.targetList, 0, &valueColumn);
			ListView_SetExtendedListViewStyle(g_app.targetList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
			g_app.targetAddFile = CreateWindowW(L"BUTTON", L"添加文件...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(27), Ui(510), Ui(116), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_TARGET_ADD_FILE), nullptr, nullptr);
			g_app.targetAddFolder = CreateWindowW(L"BUTTON", L"添加文件夹...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(153), Ui(510), Ui(128), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_TARGET_ADD_FOLDER), nullptr, nullptr);
			g_app.targetDelete = CreateWindowW(L"BUTTON", L"删除选中项", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
				Ui(488), Ui(510), Ui(135), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_TARGET_DELETE), nullptr, nullptr);
			g_app.targetSave = CreateWindowW(L"BUTTON", L"保存 CMake 目标配置", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
				Ui(638), Ui(510), Ui(231), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_TARGET_SAVE), nullptr, nullptr);
			ApplyModernTheme(categoryLabel, g_app.font); ApplyModernTheme(g_app.targetCategory, g_app.font); ApplyModernTheme(g_app.targetList, g_app.font);
			ApplyModernTheme(g_app.targetAddFile, g_app.font); ApplyModernTheme(g_app.targetAddFolder, g_app.font);
			ApplyModernTheme(g_app.targetDelete, g_app.font); ApplyModernTheme(g_app.targetSave, g_app.font);
			AddTargetPageControl(categoryLabel); AddTargetPageControl(g_app.targetCategory); AddTargetPageControl(g_app.targetList);
			AddTargetPageControl(g_app.targetAddFile); AddTargetPageControl(g_app.targetAddFolder); AddTargetPageControl(g_app.targetDelete); AddTargetPageControl(g_app.targetSave);

			g_app.status = CreateWindowW(L"STATIC", L"OpenOCD scripts 由 openocd.exe 自动查找，无需单独配置。", WS_CHILD | WS_VISIBLE, Ui(27), Ui(460), Ui(850), Ui(26), hwnd, nullptr, nullptr, nullptr);
			ApplyModernTheme(g_app.status, g_app.font);
			AddToolPageControl(g_app.status);
			PushControls();
			UpdateOpenOcdConfigControls();
			SwitchPage();
			return 0;
		}
		case WM_COMMAND:
		{
			int id = LOWORD(wParam);
			if (id == ID_TARGET_CATEGORY && HIWORD(wParam) == CBN_SELCHANGE)
			{
				UpdateTargetCategoryControls();
				return 0;
			}
			if (id == ID_TARGET_ADD_FILE)
			{
				AddCmakePath(hwnd, false);
				return 0;
			}
			if (id == ID_TARGET_ADD_FOLDER)
			{
				AddCmakePath(hwnd, true);
				return 0;
			}
			if (id == ID_TARGET_DELETE)
			{
				DeleteSelectedCmakeTargetEntry();
				return 0;
			}
			if (id == ID_TARGET_SAVE)
			{
				SaveCmakeTargetConfig(hwnd);
				return 0;
			}
			if (id == ID_OPENOCD && HIWORD(wParam) == EN_CHANGE)
			{
				UpdateOpenOcdConfigControls();
				return 0;
			}
			if (id >= ID_BROWSE_BASE && id < ID_BROWSE_BASE + 8)
			{
				Browse(hwnd, id - ID_BROWSE_BASE);
				return 0;
			}
			if (id == ID_SAVE_EXAMPLE)
			{
				if (!g_app.workspace.hasExample || Save(hwnd, true))
					DestroyWindow(hwnd);
				return 0;
			}
			if (id == ID_SAVE_SETTINGS)
			{
				if (g_app.workspace.hasExample)
				{
					if (Save(hwnd, false))
						DestroyWindow(hwnd);
				}
				else
					DestroyWindow(hwnd);
				return 0;
			}
			if (id == ID_HELP)
			{
				ShowFileHelp(hwnd);
				return 0;
			}
			if (id == ID_CLEAR)
			{
				ClearCurrentConfiguration(hwnd);
				return 0;
			}
			if (id == ID_CONFIG_SAVE_DEFAULT)
			{
				SaveUserDefaultsFromMenu(hwnd);
				return 0;
			}
			if (id == ID_CONFIG_LOAD_DEFAULT)
			{
				ReadUserDefaultsFromMenu(hwnd);
				return 0;
			}
			if (id == ID_CONFIG_CHECK_CLT)
			{
				PullControls();
				SanitizeToolPaths(g_app.tools);
				g_app.svd = g_app.tools.svd;
				PushControls();
				OfferRegistryClt(hwnd, true);
				return 0;
			}
			break;
		}
		case WM_NOTIFY:
		{
			const NMHDR* notification = reinterpret_cast<const NMHDR*>(lParam);
			if (notification && notification->idFrom == ID_PAGE_TAB && notification->code == TCN_SELCHANGE)
			{
				SwitchPage();
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetList && notification->code == NM_DBLCLK)
			{
				const NMLISTVIEW* click = reinterpret_cast<const NMLISTVIEW*>(lParam);
				if (click->iItem >= 0) ListView_EditLabel(g_app.targetList, click->iItem);
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetList && notification->code == LVN_ENDLABELEDITW)
			{
				const NMLVDISPINFOW* edit = reinterpret_cast<const NMLVDISPINFOW*>(lParam);
				if (edit->item.pszText) CommitTargetListEdit(hwnd, edit->item.iItem, edit->item.pszText);
				return 0;
			}
			break;
		}
		case WM_CLOSE:
			// 关闭窗口不需要生成配置，但必须立即让任务进程收到 WM_QUIT 并结束。
			DestroyWindow(hwnd);
			PostQuitMessage(0);
			return 0;
		case WM_CTLCOLORSTATIC:
		{
			HDC dc = reinterpret_cast<HDC>(wParam);
			SetBkMode(dc, TRANSPARENT);
			SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
			// 标签不再刷白色矩形，保留页签底色，避免出现割裂的灰白块。
			return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
		}
		case WM_DESTROY:
			if (g_app.menu) DestroyMenu(g_app.menu);
			if (g_app.font) DeleteObject(g_app.font);
			if (g_app.titleFont) DeleteObject(g_app.titleFont);
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	int RunGui(const std::wstring &root)
	{
		std::wstring discovered = FindWorkspaceRoot(root);
		const std::wstring workspaceRoot = discovered.empty() ? root : discovered;
		std::wstring error;
		if (!LoadWorkspace(workspaceRoot, g_app.workspace, error))
		{
			MessageBoxW(nullptr, error.c_str(), L"STM32 项目配置器", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return 1;
		}
		g_app.projectName = g_app.workspace.projectName;
		g_app.chipType = g_app.workspace.chipType;
		if (!LoadCMakeTargetConfig(g_app.workspace, g_app.cmakeTarget, error))
		{
			MessageBoxW(nullptr, error.c_str(), L"CMake 目标配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			g_app.cmakeTarget = {};
		}
		std::string doc;
		std::wstring settingsProject, settingsChip;
		const bool hasLocalSettings = LoadSettings(g_app.workspace.settingsPath, g_app.tools, settingsProject, settingsChip, g_app.svd, doc);
		if (g_app.chipType == L"Unknown" && !settingsChip.empty())
			g_app.chipType = settingsChip;
		// 工程 settings.json 的每个路径在使用前均做存在性与 OpenOCD cfg 合法性检查。
		SanitizeToolPaths(g_app.tools);
		g_app.svd = g_app.tools.svd;
		ApplyOpenOcdDefaults();
		// 仅当工程配置缺失或有无效工具路径时，才按默认配置、注册表的顺序补齐。
		OfferUserDefaultsOnStartup(nullptr, hasLocalSettings);
		// 默认配置只存共享工具。对于没有工程 settings.json 的首次配置，
		// 从已填入的 CubeCLT 工具反查包根目录并尝试匹配当前芯片的 SVD。
		if (!hasLocalSettings && TryAutoFillSvdFromCurrentCubeClt())
			Status(L"已从 STM32CubeCLT 自动匹配当前芯片的 SVD 文件。");
		g_app.tools.svd = g_app.svd;
		HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSW wc{};
		wc.hInstance = instance;
		wc.lpfnWndProc = WindowProc;
		wc.lpszClassName = L"PathConfiguratorWindow";
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
		RegisterClassW(&wc);
		constexpr DWORD windowStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
		// 控件坐标以客户区 912 宽为基准，补偿非客户区宽度以使左右留白对等。
		RECT desiredClient{0, 0, Ui(912), 0};
		AdjustWindowRectEx(&desiredClient, windowStyle, TRUE, WS_EX_APPWINDOW);
		const int windowWidth = desiredClient.right - desiredClient.left;
		HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, (g_app.workspace.projectLabel + L" - STM32 工具链配置").c_str(),
																windowStyle,
																CW_USEDEFAULT, CW_USEDEFAULT, windowWidth, Ui(680), nullptr, nullptr, instance, nullptr);
		if (!hwnd)
			return 1;
		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);
		// 窗口显示后再次同步，确保原生 Edit 控件已经完成创建。
		PushControls();
		MSG msg{};
		while (GetMessageW(&msg, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
		return 0;
	}

	int RunValidate(const std::wstring &root)
	{
		std::wstring discovered = FindWorkspaceRoot(root);
		const std::wstring workspaceRoot = discovered.empty() ? root : discovered;
		WorkspaceInfo info;
		std::wstring error;
		if (!LoadWorkspace(workspaceRoot, info, error))
		{
			MessageBoxW(nullptr, error.c_str(), L"验证失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return 1;
		}
		ToolPaths tools;
		std::wstring project, chip, svd;
		std::string doc;
		if (!LoadSettings(info.settingsPath, tools, project, chip, svd, doc))
		{
			MessageBoxW(nullptr, L"未找到或无法读取 .vscode/settings.json。", L"验证失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return 1;
		}
		ValidationResult result = ValidateTools(tools, false);
	MessageBoxW(nullptr, result.message.c_str(), result.ok ? L"验证通过" : L"验证失败", MB_OK | (result.ok ? MB_ICONINFORMATION : MB_ICONERROR) | MB_TOPMOST);
		return result.ok ? 0 : 1;
	}

} // namespace

int wmain()
{
	// 从资源管理器直接启动时不显示短暂的控制台窗口；ConPTY 环境下不会影响 VS Code 终端。
	if (HWND console = GetConsoleWindow())
		ShowWindow(console, SW_HIDE);

	INITCOMMONCONTROLSEX commonControls{};
	commonControls.dwSize = sizeof(commonControls);
	commonControls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES;
	InitCommonControlsEx(&commonControls);
	OleInitialize(nullptr);
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	wchar_t cwd[MAX_PATH * 4]{};
	if (GetCurrentDirectoryW(static_cast<DWORD>(ARRAY_SIZE(cwd)), cwd) == 0)
		GetModuleFileNameW(nullptr, cwd, static_cast<DWORD>(ARRAY_SIZE(cwd)));
	std::wstring root = cwd;
	if (FindWorkspaceRoot(root).empty())
	{
		wchar_t modulePath[MAX_PATH * 4]{};
		GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(ARRAY_SIZE(modulePath)));
		std::wstring moduleRoot = GetParentPath(modulePath);
		if (!moduleRoot.empty()) root = moduleRoot;
	}
	bool validate = false;
	for (int i = 1; i < argc; ++i)
	{
		std::wstring arg = argv[i];
		if (arg == L"/help" || arg == L"-help" || arg == L"--help")
		{
			MessageBoxW(nullptr, L"STM32 项目配置器\r\n\r\n用法：\r\n  PathConfigurator.exe                 打开配置窗口\r\n  PathConfigurator.exe /workspace <目录> 打开指定工程\r\n  PathConfigurator.exe /validate <目录>  验证 settings.json", L"帮助", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
			if (argv)
				LocalFree(argv);
			OleUninitialize();
			return 0;
		}
		if (arg == L"/validate" || arg == L"-validate")
		{
			validate = true;
			if (i + 1 < argc)
				root = argv[++i];
		}
		else if (arg == L"/workspace" || arg == L"-workspace")
		{
			if (i + 1 < argc)
				root = argv[++i];
		}
	}
	int result = validate ? RunValidate(root) : RunGui(root);
	if (argv)
		LocalFree(argv);
	OleUninitialize();
	return result;
}
