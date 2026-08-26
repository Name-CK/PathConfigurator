#include "core.hpp"

#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

using namespace pathconfig;

namespace
{
	// 统一逻辑尺寸。进程声明为系统 DPI 感知后，按系统缩放创建真实像素尺寸，避免 DWM 位图放大客户端文字。
	UINT g_systemDpi = 96;
	int Ui(int value)
	{
		return MulDiv((value * 22 + 12) / 25, static_cast<int>(g_systemDpi), 96);
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
		ID_OTHER_ABOUT = 302,
		ID_CONFIG_SAVE_DEFAULT = 303,
		ID_CONFIG_LOAD_DEFAULT = 304,
		ID_CONFIG_CHECK_CLT = 305,
		ID_CLEAR = 306,
		ID_PAGE_TAB = 320,
		ID_TARGET_CATEGORY = 321,
		ID_TARGET_ADD_FILE = 322,
		ID_TARGET_ADD_FOLDER = 323,
		ID_TARGET_DELETE = 324,
		ID_TARGET_MOVE_UP = 325,
		ID_TARGET_MOVE_DOWN = 326,
		ID_TARGET_SAVE = 327,
		ID_UPDATE_LINK = 328,
		ID_CONFIG_MENU_BUTTON = 329,
		ID_OTHER_MENU_BUTTON = 330,
		ID_TARGET_ADD_VIRTUAL_FOLDER = 331,
		ID_TARGET_TREE = 332,
		ID_ABOUT_REPOSITORY = 333,
		ID_ABOUT_STATUS = 334,
		ID_ABOUT_CLOSE = 335
	};

	constexpr UINT WM_APP_CMAKE_DIALOG_RESULT = WM_APP + 2;
	constexpr UINT WM_APP_CMAKE_DIALOG_CLEANUP_COMPLETE = WM_APP + 3;
	constexpr UINT_PTR ID_HOVER_TOOLTIP_TIMER = 1;
	constexpr UINT_PTR ID_UPDATE_PROCESS_TIMER = 2;
	constexpr UINT_PTR ID_ABOUT_UPDATE_TIMER = 3;
	constexpr int CUSTOM_MENU_HEIGHT = 20;

	constexpr wchar_t kAppVersion[] = L"1.0.0";
	constexpr wchar_t kGitHubRepository[] = L"https://github.com/Name-CK/PathConfigurator";
	constexpr wchar_t kGitHubReleasePage[] = L"https://github.com/Name-CK/PathConfigurator/releases";

	struct LatestRelease
	{
		std::wstring tag;
		std::wstring url;
	};

	struct CMakeDialogRequest
	{
		HWND owner = nullptr;
		std::wstring root;
		std::wstring title;
		bool isFolder = false;
	};

	struct CMakeDialogResult
	{
		std::vector<std::wstring> paths;
		bool isFolder = false;
	};

	enum class TargetTreeItemKind
	{
		Root,
		VirtualFolder,
		Source,
		PhysicalSourceFolder,
		PhysicalFolder
	};

	struct TargetTreeItem
	{
		TargetTreeItemKind kind = TargetTreeItemKind::Root;
		std::wstring virtualFolder;
		size_t sourceIndex = static_cast<size_t>(-1);
		std::wstring physicalPath;
		bool physicalChildrenLoaded = false;
	};

	struct PhysicalBrowserEntry
	{
		std::wstring path;
		bool isFolder = false;
	};

	struct AppState
	{
		WorkspaceInfo workspace;
		ToolPaths tools;
		std::wstring projectName;
		std::wstring chipType;
		std::wstring svd;
		std::wstring updateUrl;
		CMakeTargetConfig cmakeTarget;
		HWND edits[8]{};
		HWND openocdConfigBrowse[2]{};
		HWND pageTab = nullptr;
		HWND targetCategory = nullptr;
		HWND targetTree = nullptr;
		HWND targetList = nullptr;
		HWND targetAddVirtualFolder = nullptr;
		HWND targetAddFile = nullptr;
		HWND targetAddFolder = nullptr;
		HWND targetDelete = nullptr;
		HWND targetMoveUp = nullptr;
		HWND targetMoveDown = nullptr;
		HWND targetSave = nullptr;
		HWND tooltip = nullptr;
		HWND hoverControl = nullptr;
		std::vector<std::pair<HWND, std::wstring>> tooltipTexts;
		HWND updateLink = nullptr;
		std::vector<HWND> toolPageControls;
		std::vector<HWND> targetPageControls;
		HWND status = nullptr;
		HWND window = nullptr;
		HFONT font = nullptr;
		HFONT titleFont = nullptr;
		ID2D1Factory* d2dFactory = nullptr;
		ID2D1DCRenderTarget* d2dDcTarget = nullptr;
		IDWriteFactory* dwriteFactory = nullptr;
		IDWriteTextFormat* dwriteFont = nullptr;
		IDWriteTextFormat* dwriteTitleFont = nullptr;
		HBRUSH windowBrush = nullptr;
		HBRUSH controlBrush = nullptr;
		HBRUSH panelBrush = nullptr;
		HMENU menu = nullptr;
		HMENU configMenu = nullptr;
		HMENU otherMenu = nullptr;
		HWND configMenuButton = nullptr;
		HWND otherMenuButton = nullptr;
		bool updateCheckStarted = false;
		HANDLE updateProcess = nullptr;
		bool cmakeDialogOpen = false;
		unsigned int cmakeDialogWorkers = 0;
		std::vector<std::unique_ptr<TargetTreeItem>> targetTreeItems;
		std::vector<size_t> targetSourceRows;
		std::vector<PhysicalBrowserEntry> physicalBrowserRows;
		std::wstring selectedVirtualFolder;
		std::wstring pendingVirtualFolderParent;
		std::wstring renamingVirtualFolder;
		bool addingVirtualFolder = false;
		bool targetListReadOnly = false;
		// LoadWorkspace 已确认 CubeMX CMake 工程；工具全部有效时默认打开目标配置页。
		bool openCmakeTargetOnStartup = false;
	};

	enum class AboutUpdateState
	{
		Checking,
		Latest,
		Outdated,
		Failed
	};

	struct AboutWindowState
	{
		HWND window = nullptr;
		HWND status = nullptr;
		HANDLE updateProcess = nullptr;
		std::wstring updateUrl;
		AboutUpdateState updateState = AboutUpdateState::Checking;
	};

	AppState g_app;
	AboutWindowState g_about;

	// 默认深色主题：所有颜色均由 GDI 直接绘制，不依赖额外图片资源。
	constexpr COLORREF kThemeWindow = RGB(30, 30, 30);
	constexpr COLORREF kThemePanel = RGB(37, 37, 38);
	constexpr COLORREF kThemeControl = RGB(45, 45, 48);
	constexpr COLORREF kThemeBorder = RGB(70, 70, 74);
	constexpr COLORREF kThemeText = RGB(232, 232, 232);
	constexpr COLORREF kThemeMutedText = RGB(150, 150, 150);
	constexpr COLORREF kThemeAccent = RGB(0, 122, 204);
	constexpr COLORREF kThemeSelection = RGB(38, 79, 120);
	constexpr COLORREF kThemeHover = RGB(55, 55, 58);
	const IID kIdWriteFactoryIid = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

	void EnableSystemDpiAwareness()
	{
		using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
		using GetDpiForSystemFn = UINT(WINAPI*)();
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32)
		{
			auto setDpiAwareness = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
				GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
			// -2 等价于 DPI_AWARENESS_CONTEXT_SYSTEM_AWARE；保持静态布局的同时消除系统位图缩放。
			if (setDpiAwareness)
				setDpiAwareness(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-2)));
			auto getDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem"));
			if (getDpiForSystem)
			{
				const UINT dpi = getDpiForSystem();
				if (dpi != 0) g_systemDpi = dpi;
				return;
			}
		}
		// Windows 7/旧版 Windows 的兼容回退。
		SetProcessDPIAware();
	}

	enum class ThemedTextAlign
	{
		Left,
		Center,
		Right
	};

	void ReleaseDirectWriteResources()
	{
		if (g_app.dwriteTitleFont) { g_app.dwriteTitleFont->Release(); g_app.dwriteTitleFont = nullptr; }
		if (g_app.dwriteFont) { g_app.dwriteFont->Release(); g_app.dwriteFont = nullptr; }
		if (g_app.dwriteFactory) { g_app.dwriteFactory->Release(); g_app.dwriteFactory = nullptr; }
		if (g_app.d2dDcTarget) { g_app.d2dDcTarget->Release(); g_app.d2dDcTarget = nullptr; }
		if (g_app.d2dFactory) { g_app.d2dFactory->Release(); g_app.d2dFactory = nullptr; }
	}

	// DirectWrite 使用 Windows 的文本排版和字形栅格化；若系统组件不可用则保留 GDI 回退。
	bool InitializeDirectWriteResources()
	{
		ReleaseDirectWriteResources();
		if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_app.d2dFactory)) ||
			FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, kIdWriteFactoryIid,
				reinterpret_cast<IUnknown**>(&g_app.dwriteFactory))))
		{
			ReleaseDirectWriteResources();
			return false;
		}

		D2D1_RENDER_TARGET_PROPERTIES targetProperties{};
		targetProperties.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
		targetProperties.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
		targetProperties.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
		targetProperties.dpiX = 96.0f;
		targetProperties.dpiY = 96.0f;
		targetProperties.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
		targetProperties.minLevel = D2D1_FEATURE_LEVEL_DEFAULT;
		if (FAILED(g_app.d2dFactory->CreateDCRenderTarget(&targetProperties, &g_app.d2dDcTarget)) ||
			FAILED(g_app.dwriteFactory->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
				DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
				static_cast<FLOAT>(Ui(16)), L"zh-CN", &g_app.dwriteFont)) ||
			FAILED(g_app.dwriteFactory->CreateTextFormat(L"Microsoft YaHei", nullptr,
				DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
				static_cast<FLOAT>(Ui(20)), L"zh-CN", &g_app.dwriteTitleFont)))
		{
			ReleaseDirectWriteResources();
			return false;
		}
		return true;
	}

	bool DrawDirectWriteText(HDC dc, const wchar_t* text, const RECT& rect, COLORREF color,
		ThemedTextAlign align = ThemedTextAlign::Left, bool wrap = false, bool ellipsis = false, bool title = false)
	{
		if (!dc || !text || !*text || !g_app.d2dDcTarget || !g_app.dwriteFactory)
			return false;
		const int width = rect.right - rect.left;
		const int height = rect.bottom - rect.top;
		if (width <= 0 || height <= 0)
			return false;
		IDWriteTextFormat* format = title ? g_app.dwriteTitleFont : g_app.dwriteFont;
		if (!format)
			return false;

		IDWriteTextLayout* layout = nullptr;
		if (FAILED(g_app.dwriteFactory->CreateTextLayout(text, lstrlenW(text), format,
			static_cast<FLOAT>(width), static_cast<FLOAT>(height), &layout)))
			return false;
		layout->SetTextAlignment(align == ThemedTextAlign::Center ? DWRITE_TEXT_ALIGNMENT_CENTER :
			(align == ThemedTextAlign::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING));
		layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
		layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
		if (ellipsis)
		{
			DWRITE_TRIMMING trimming{};
			trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
			IDWriteInlineObject* trimmingSign = nullptr;
			if (SUCCEEDED(g_app.dwriteFactory->CreateEllipsisTrimmingSign(format, &trimmingSign)))
			{
				layout->SetTrimming(&trimming, trimmingSign);
				trimmingSign->Release();
			}
		}

		RECT bindRect = rect;
		HRESULT result = g_app.d2dDcTarget->BindDC(dc, &bindRect);
		if (SUCCEEDED(result))
		{
			g_app.d2dDcTarget->BeginDraw();
			g_app.d2dDcTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
			D2D1_COLOR_F textColor{};
			textColor.r = static_cast<FLOAT>(GetRValue(color)) / 255.0f;
			textColor.g = static_cast<FLOAT>(GetGValue(color)) / 255.0f;
			textColor.b = static_cast<FLOAT>(GetBValue(color)) / 255.0f;
			textColor.a = 1.0f;
			ID2D1SolidColorBrush* brush = nullptr;
			result = g_app.d2dDcTarget->CreateSolidColorBrush(&textColor, nullptr, &brush);
			if (SUCCEEDED(result))
			{
				const D2D1_POINT_2F origin{0.0f, 0.0f};
				g_app.d2dDcTarget->DrawTextLayout(origin, layout, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
				brush->Release();
			}
			const HRESULT endResult = g_app.d2dDcTarget->EndDraw();
			if (endResult == D2DERR_RECREATE_TARGET)
			{
				g_app.d2dDcTarget->Release();
				g_app.d2dDcTarget = nullptr;
			}
			else if (FAILED(endResult))
				result = endResult;
		}
		layout->Release();
		return SUCCEEDED(result);
	}

	void DrawThemedText(HDC dc, const wchar_t* text, const RECT& rect, COLORREF color,
		ThemedTextAlign align = ThemedTextAlign::Left, bool wrap = false, bool ellipsis = false, bool title = false)
	{
		if (DrawDirectWriteText(dc, text, rect, color, align, wrap, ellipsis, title))
			return;
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, color);
		HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, title ? g_app.titleFont : g_app.font));
		UINT flags = (align == ThemedTextAlign::Center ? DT_CENTER :
			(align == ThemedTextAlign::Right ? DT_RIGHT : DT_LEFT)) | DT_VCENTER | DT_NOPREFIX;
		flags |= wrap ? DT_WORDBREAK : DT_SINGLELINE;
		if (ellipsis) flags |= DT_END_ELLIPSIS;
		RECT fallbackRect = rect;
		DrawTextW(dc, text, -1, &fallbackRect, flags);
		SelectObject(dc, oldFont);
	}

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

	// 虚拟文件夹是工程配置内的分组路径；统一使用 /，不与实际磁盘目录混淆。
	std::wstring NormalizeVirtualFolderPath(const std::wstring& value)
	{
		std::wstring path = Trim(value);
		for (wchar_t& c : path) if (c == L'\\') c = L'/';
		std::wstring normalized;
		size_t begin = 0;
		while (begin <= path.size())
		{
			const size_t end = path.find(L'/', begin);
			const std::wstring segment = Trim(path.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin));
			if (segment.empty()) return {};
			if (!normalized.empty()) normalized += L'/';
			normalized += segment;
			if (end == std::wstring::npos) break;
			begin = end + 1;
		}
		return normalized;
	}

	bool IsValidVirtualFolderPath(const std::wstring& rawValue)
	{
		const std::wstring value = NormalizeVirtualFolderPath(rawValue);
		if (value.empty() || value.front() == L'/' || value.back() == L'/') return false;
		size_t begin = 0;
		while (begin < value.size())
		{
			const size_t end = value.find(L'/', begin);
			const std::wstring segment = value.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
			if (segment == L"." || segment == L"..") return false;
			for (wchar_t c : segment)
				if (c < 32 || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' ||
					c == L'|' || c == L';' || c == L'$') return false;
			if (end == std::wstring::npos) break;
			begin = end + 1;
		}
		return true;
	}

	std::wstring ParentVirtualFolder(const std::wstring& path)
	{
		const size_t separator = path.rfind(L'/');
		return separator == std::wstring::npos ? L"" : path.substr(0, separator);
	}

	std::wstring VirtualFolderLeafName(const std::wstring& path)
	{
		const size_t separator = path.rfind(L'/');
		return separator == std::wstring::npos ? path : path.substr(separator + 1);
	}

	bool SameVirtualFolder(const std::wstring& left, const std::wstring& right)
	{
		return SameConfigValue(NormalizeVirtualFolderPath(left), NormalizeVirtualFolderPath(right));
	}

	bool IsVirtualFolderOrDescendant(const std::wstring& value, const std::wstring& parent)
	{
		const std::wstring normalizedValue = NormalizeVirtualFolderPath(value);
		const std::wstring normalizedParent = NormalizeVirtualFolderPath(parent);
		if (normalizedParent.empty()) return true;
		if (SameVirtualFolder(normalizedValue, normalizedParent)) return true;
		return normalizedValue.size() > normalizedParent.size() &&
			_wcsnicmp(normalizedValue.c_str(), normalizedParent.c_str(), normalizedParent.size()) == 0 &&
			normalizedValue[normalizedParent.size()] == L'/';
	}

	bool IsPhysicalTreeFolder(const TargetTreeItem* item)
	{
		return item && (item->kind == TargetTreeItemKind::PhysicalSourceFolder ||
			item->kind == TargetTreeItemKind::PhysicalFolder);
	}

	TargetTreeItem* TargetTreeItemFromHandle(HTREEITEM handle)
	{
		if (!g_app.targetTree || !handle) return nullptr;
		TVITEMW item{};
		item.mask = TVIF_PARAM;
		item.hItem = handle;
		return TreeView_GetItem(g_app.targetTree, &item) ? reinterpret_cast<TargetTreeItem*>(item.lParam) : nullptr;
	}

	TargetTreeItem* SelectedTargetTreeItem()
	{
		return TargetTreeItemFromHandle(g_app.targetTree ? TreeView_GetSelection(g_app.targetTree) : nullptr);
	}

	std::wstring CurrentVirtualFolder()
	{
		if (TargetTreeItem* item = SelectedTargetTreeItem())
			return NormalizeVirtualFolderPath(item->virtualFolder);
		return NormalizeVirtualFolderPath(g_app.selectedVirtualFolder);
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

	size_t TargetEntryCount(int category)
	{
		switch (category)
		{
		case TargetSources: return g_app.targetListReadOnly ? g_app.physicalBrowserRows.size() : g_app.targetSourceRows.size();
		case TargetIncludes: return g_app.cmakeTarget.includeDirectories.size();
		case TargetDefinitions: return g_app.cmakeTarget.compileDefinitions.size();
		case TargetLinkDirectories: return g_app.cmakeTarget.linkDirectories.size();
		default: return 0;
		}
	}

	std::vector<int> SelectedTargetListRows()
	{
		std::vector<int> rows;
		if (!g_app.targetList) return rows;
		const size_t entryCount = TargetEntryCount(SelectedTargetCategory());
		for (int index = ListView_GetNextItem(g_app.targetList, -1, LVNI_SELECTED);
			index >= 0;
			index = ListView_GetNextItem(g_app.targetList, index, LVNI_SELECTED))
		{
			if (static_cast<size_t>(index) < entryCount) rows.push_back(index);
		}
		return rows;
	}

	std::vector<int> SelectedTargetEntryIndices()
	{
		std::vector<int> indices;
		const std::vector<int> rows = SelectedTargetListRows();
		if (SelectedTargetCategory() == TargetSources)
		{
			for (int row : rows)
				if (static_cast<size_t>(row) < g_app.targetSourceRows.size())
					indices.push_back(static_cast<int>(g_app.targetSourceRows[static_cast<size_t>(row)]));
		}
		else indices = rows;
		return indices;
	}

	void UpdateTargetActionButtons()
	{
		if (!g_app.targetList)
			return;
		const bool sourceCategory = SelectedTargetCategory() == TargetSources;
		const std::vector<int> selectedRows = SelectedTargetListRows();
		const bool hasEntry = !selectedRows.empty();
		TargetTreeItem* selectedTreeItem = sourceCategory ? SelectedTargetTreeItem() : nullptr;
		if (sourceCategory)
		{
			const bool editable = !g_app.targetListReadOnly;
			// 源目录和子目录仅用于查看实际内容；隐藏所有会修改工程配置的新增按钮。
			if (g_app.targetAddVirtualFolder) {
				EnableWindow(g_app.targetAddVirtualFolder, editable);
				ShowWindow(g_app.targetAddVirtualFolder, editable ? SW_SHOW : SW_HIDE);
			}
			if (g_app.targetAddFile) {
				EnableWindow(g_app.targetAddFile, editable);
				ShowWindow(g_app.targetAddFile, editable ? SW_SHOW : SW_HIDE);
			}
			if (g_app.targetAddFolder) {
				EnableWindow(g_app.targetAddFolder, editable);
				ShowWindow(g_app.targetAddFolder, editable ? SW_SHOW : SW_HIDE);
			}
			if (!editable)
			{
				const bool canDeleteTreeItem = selectedTreeItem && IsPhysicalTreeFolder(selectedTreeItem);
				if (g_app.targetDelete) EnableWindow(g_app.targetDelete, hasEntry || canDeleteTreeItem);
				if (g_app.targetMoveUp) EnableWindow(g_app.targetMoveUp, FALSE);
				if (g_app.targetMoveDown) EnableWindow(g_app.targetMoveDown, FALSE);
				if (g_app.targetSave) EnableWindow(g_app.targetSave, TRUE);
				return;
			}
		}
		const size_t count = TargetEntryCount(SelectedTargetCategory());
		const bool canDeleteTreeItem = selectedTreeItem &&
			((selectedTreeItem->kind == TargetTreeItemKind::VirtualFolder && !selectedTreeItem->virtualFolder.empty()) ||
			 (selectedTreeItem->kind == TargetTreeItemKind::Source && selectedTreeItem->sourceIndex != static_cast<size_t>(-1)));
		if (g_app.targetDelete) EnableWindow(g_app.targetDelete, hasEntry || (!hasEntry && canDeleteTreeItem));
		if (g_app.targetMoveUp) EnableWindow(g_app.targetMoveUp, hasEntry && selectedRows.front() > 0);
		if (g_app.targetMoveDown) EnableWindow(g_app.targetMoveDown, hasEntry && static_cast<size_t>(selectedRows.back() + 1) < count);
		if (g_app.targetSave) EnableWindow(g_app.targetSave, TRUE);
	}

	void AddToolTip(HWND control, const wchar_t* text)
	{
		if (!control)
			return;
		for (const auto& item : g_app.tooltipTexts)
			if (item.first == control)
				return;
		g_app.tooltipTexts.emplace_back(control, text);
	}

	void UpdateToolTip(HWND control, const wchar_t* text)
	{
		if (!control)
			return;
		for (auto& item : g_app.tooltipTexts)
			if (item.first == control)
				item.second = text;
	}

	const wchar_t* ToolTipText(HWND control)
	{
		for (const auto& item : g_app.tooltipTexts)
			if (item.first == control)
				return item.second.c_str();
		return nullptr;
	}

	LRESULT CALLBACK HoverToolTipProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_ERASEBKGND:
			return 1;
		case WM_NCHITTEST:
			return HTTRANSPARENT;
		case WM_PAINT:
		{
			PAINTSTRUCT paint{};
			HDC dc = BeginPaint(hwnd, &paint);
			RECT rect{};
			GetClientRect(hwnd, &rect);
			FillRect(dc, &rect, g_app.controlBrush);
			HPEN pen = CreatePen(PS_SOLID, 1, kThemeBorder);
			HGDIOBJ oldPen = SelectObject(dc, pen);
			HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
			Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
			InflateRect(&rect, -Ui(7), -Ui(4));
			wchar_t text[512]{};
			GetWindowTextW(hwnd, text, static_cast<int>(ARRAY_SIZE(text)));
			DrawThemedText(dc, text, rect, kThemeText, ThemedTextAlign::Left, true);
			EndPaint(hwnd, &paint);
			return 0;
		}
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	// 标题、项目摘要和更新链接均在客户区绘制，避免原生 STATIC 控件回退到 GDI 位图缩放。
	LRESULT CALLBACK DirectWriteStaticSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR, DWORD_PTR data)
	{
		if (message == WM_ERASEBKGND)
			return 1;
		if (message == WM_PAINT)
		{
			PAINTSTRUCT paint{};
			HDC dc = BeginPaint(hwnd, &paint);
			RECT rect{};
			GetClientRect(hwnd, &rect);
			FillRect(dc, &rect, g_app.windowBrush);
			wchar_t text[512]{};
			GetWindowTextW(hwnd, text, static_cast<int>(ARRAY_SIZE(text)));
			const bool isTitle = data == 1;
			const bool isUpdateLink = data == 3;
			const bool isAboutStatus = data == 4;
			const bool isRepositoryLink = data == 5;
			COLORREF color = kThemeText;
			if (isUpdateLink) color = RGB(255, 106, 106);
			else if (isRepositoryLink) color = RGB(105, 185, 245);
			else if (isAboutStatus)
			{
				switch (g_about.updateState)
				{
				case AboutUpdateState::Latest: color = RGB(126, 205, 135); break;
				case AboutUpdateState::Outdated: color = RGB(255, 126, 126); break;
				case AboutUpdateState::Failed: color = RGB(225, 183, 92); break;
				default: color = kThemeMutedText; break;
				}
			}
			DrawThemedText(dc, text, rect, color,
				isUpdateLink ? ThemedTextAlign::Right : ThemedTextAlign::Left,
				isAboutStatus, !isAboutStatus, isTitle);
			EndPaint(hwnd, &paint);
			return 0;
		}
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}

	void HideToolTip()
	{
		if (g_app.window)
			KillTimer(g_app.window, ID_HOVER_TOOLTIP_TIMER);
		if (g_app.tooltip)
			ShowWindow(g_app.tooltip, SW_HIDE);
		g_app.hoverControl = nullptr;
	}

	void QueueDarkMenuSeparatorRepaint(HWND hwnd)
	{
		// 顶层菜单已改为客户区自绘控件；保留调用点以兼容各选择器返回流程。
		if (hwnd)
		{
			RECT menuBar{0, 0, Ui(912), Ui(CUSTOM_MENU_HEIGHT)};
			InvalidateRect(hwnd, &menuBar, TRUE);
		}
	}

	void BeginToolTip(HWND control)
	{
		if (!ToolTipText(control))
			return;
		if (g_app.hoverControl == control)
			return;
		HideToolTip();
		g_app.hoverControl = control;
		SetTimer(g_app.window, ID_HOVER_TOOLTIP_TIMER, 450, nullptr);
	}

	void ShowToolTip()
	{
		const wchar_t* text = ToolTipText(g_app.hoverControl);
		if (!g_app.tooltip || !text)
			return;
		POINT point{};
		GetCursorPos(&point);
		if (WindowFromPoint(point) != g_app.hoverControl)
		{
			HideToolTip();
			return;
		}
		SetWindowTextW(g_app.tooltip, text);
		HDC dc = GetDC(g_app.tooltip);
		RECT textRect{0, 0, Ui(280), 0};
		HFONT oldFont = reinterpret_cast<HFONT>(SelectObject(dc, g_app.font));
		DrawTextW(dc, text, -1, &textRect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
		SelectObject(dc, oldFont);
		ReleaseDC(g_app.tooltip, dc);
		int width = textRect.right - textRect.left + Ui(14);
		if (width < Ui(120)) width = Ui(120);
		if (width > Ui(300)) width = Ui(300);
		const int height = textRect.bottom - textRect.top + Ui(10);
		SetWindowPos(g_app.tooltip, HWND_TOPMOST, point.x + Ui(14), point.y + Ui(20), width, height,
			SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}

	TargetTreeItem* NewTargetTreeItem(TargetTreeItemKind kind, const std::wstring& virtualFolder,
		size_t sourceIndex = static_cast<size_t>(-1), const std::wstring& physicalPath = L"")
	{
		auto item = std::make_unique<TargetTreeItem>();
		item->kind = kind;
		item->virtualFolder = NormalizeVirtualFolderPath(virtualFolder);
		item->sourceIndex = sourceIndex;
		item->physicalPath = NormalizePath(physicalPath);
		TargetTreeItem* raw = item.get();
		g_app.targetTreeItems.push_back(std::move(item));
		return raw;
	}

	HTREEITEM InsertTargetTreeItem(HTREEITEM parent, const std::wstring& text, TargetTreeItem* data, bool bold = false)
	{
		TVINSERTSTRUCTW insertion{};
		insertion.hParent = parent;
		insertion.hInsertAfter = TVI_LAST;
		insertion.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
		insertion.item.pszText = const_cast<wchar_t*>(text.c_str());
		insertion.item.lParam = reinterpret_cast<LPARAM>(data);
		insertion.item.stateMask = TVIS_BOLD;
		insertion.item.state = bold ? TVIS_BOLD : 0;
		return TreeView_InsertItem(g_app.targetTree, &insertion);
	}

	bool PhysicalFolderHasDirectChildFolder(const std::wstring& path)
	{
		WIN32_FIND_DATAW data{};
		HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE) return false;
		bool found = false;
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
				(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
				wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
				continue;
			found = true;
			break;
		} while (FindNextFileW(find, &data));
		FindClose(find);
		return found;
	}

	void AddPhysicalFolderPlaceholder(HTREEITEM parent, const std::wstring& path)
	{
		if (!PhysicalFolderHasDirectChildFolder(path)) return;
		// 仅告诉 TreeView 此节点可展开，不插入不可见的伪子节点，避免留下图像占位方框。
		TVITEMW item{};
		item.mask = TVIF_CHILDREN;
		item.hItem = parent;
		item.cChildren = 1;
		TreeView_SetItem(g_app.targetTree, &item);
	}

	void LoadPhysicalFolderChildren(HTREEITEM parent, TargetTreeItem* item)
	{
		if (!IsPhysicalTreeFolder(item) || item->physicalChildrenLoaded) return;
		while (HTREEITEM child = TreeView_GetChild(g_app.targetTree, parent))
			TreeView_DeleteItem(g_app.targetTree, child);
		item->physicalChildrenLoaded = true;

		std::vector<std::pair<std::wstring, std::wstring>> folders;
		WIN32_FIND_DATAW data{};
		HANDLE find = FindFirstFileW(JoinPath(item->physicalPath, L"*").c_str(), &data);
		if (find == INVALID_HANDLE_VALUE) return;
		do
		{
			if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
				(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
				wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
				continue;
			folders.emplace_back(data.cFileName, JoinPath(item->physicalPath, data.cFileName));
		} while (FindNextFileW(find, &data));
		FindClose(find);
		std::sort(folders.begin(), folders.end(), [](const auto& left, const auto& right) {
			return _wcsicmp(left.first.c_str(), right.first.c_str()) < 0;
		});
		for (const auto& folder : folders)
		{
			TargetTreeItem* child = NewTargetTreeItem(TargetTreeItemKind::PhysicalFolder, item->virtualFolder,
				static_cast<size_t>(-1), folder.second);
			const HTREEITEM node = InsertTargetTreeItem(parent, folder.first, child);
			AddPhysicalFolderPlaceholder(node, child->physicalPath);
		}
	}

	void RefreshTargetTree()
	{
		if (!g_app.targetTree) return;
		const std::wstring requestedSelection = CurrentVirtualFolder();
		if (!requestedSelection.empty()) g_app.selectedVirtualFolder = requestedSelection;
		TreeView_DeleteAllItems(g_app.targetTree);
		g_app.targetTreeItems.clear();

		const HTREEITEM root = InsertTargetTreeItem(TVI_ROOT, L"项目根目录", NewTargetTreeItem(TargetTreeItemKind::Root, L""), true);
		std::vector<std::pair<std::wstring, HTREEITEM>> folders;
		folders.emplace_back(L"", root);
		std::function<HTREEITEM(const std::wstring&)> ensureFolder;
		ensureFolder = [&](const std::wstring& rawPath) -> HTREEITEM
		{
			const std::wstring path = NormalizeVirtualFolderPath(rawPath);
			if (path.empty()) return root;
			for (const auto& existing : folders)
				if (SameVirtualFolder(existing.first, path)) return existing.second;
			const HTREEITEM parent = ensureFolder(ParentVirtualFolder(path));
			const HTREEITEM node = InsertTargetTreeItem(parent, VirtualFolderLeafName(path),
				NewTargetTreeItem(TargetTreeItemKind::VirtualFolder, path), true);
			folders.emplace_back(path, node);
			return node;
		};

		for (const std::wstring& folder : g_app.cmakeTarget.virtualFolders)
			ensureFolder(folder);
		for (const TargetSourceEntry& source : g_app.cmakeTarget.sources)
			if (!source.virtualFolder.empty()) ensureFolder(source.virtualFolder);
		for (size_t i = 0; i < g_app.cmakeTarget.sources.size(); ++i)
		{
			const TargetSourceEntry& source = g_app.cmakeTarget.sources[i];
			const HTREEITEM parent = ensureFolder(source.virtualFolder);
			if (source.isFolder)
			{
				const std::wstring physicalPath = NormalizePath(JoinPath(g_app.workspace.toolchainRoot, source.path));
				TargetTreeItem* item = NewTargetTreeItem(TargetTreeItemKind::PhysicalSourceFolder, source.virtualFolder, i, physicalPath);
				const HTREEITEM node = InsertTargetTreeItem(parent, ProjectRelativeDisplayPath(source.path, true), item, true);
				AddPhysicalFolderPlaceholder(node, physicalPath);
			}
			else
			{
				InsertTargetTreeItem(parent, ProjectRelativeDisplayPath(source.path, false),
					NewTargetTreeItem(TargetTreeItemKind::Source, source.virtualFolder, i));
			}
		}
		TreeView_Expand(g_app.targetTree, root, TVE_EXPAND);
		for (const auto& folder : folders)
			if (!folder.first.empty()) TreeView_Expand(g_app.targetTree, folder.second, TVE_EXPAND);

		HTREEITEM selected = root;
		for (const auto& folder : folders)
			if (SameVirtualFolder(folder.first, g_app.selectedVirtualFolder)) { selected = folder.second; break; }
		TreeView_SelectItem(g_app.targetTree, selected);
	}

	void SetTargetListHeader(const wchar_t* text)
	{
		if (!g_app.targetList) return;
		LVCOLUMNW column{};
		column.mask = LVCF_TEXT;
		column.pszText = const_cast<wchar_t*>(text);
		ListView_SetColumn(g_app.targetList, 0, &column);
	}

	void RefreshPhysicalFolderContents(const std::wstring& path)
	{
		struct Entry { std::wstring name; bool isFolder = false; std::wstring path; };
		std::vector<Entry> entries;
		WIN32_FIND_DATAW data{};
		HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
		if (find != INVALID_HANDLE_VALUE)
		{
			do
			{
				if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
				entries.push_back({data.cFileName, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
					JoinPath(path, data.cFileName)});
			} while (FindNextFileW(find, &data));
			FindClose(find);
		}
		std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
			if (left.isFolder != right.isFolder) return left.isFolder;
			return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
		});
		g_app.physicalBrowserRows.clear();
		for (size_t i = 0; i < entries.size(); ++i)
		{
			g_app.physicalBrowserRows.push_back({entries[i].path, entries[i].isFolder});
			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = static_cast<int>(i);
			std::wstring text = (entries[i].isFolder ? L"[目录] " : L"[文件] ") + entries[i].name;
			item.pszText = text.data();
			ListView_InsertItem(g_app.targetList, &item);
		}
	}

	void RefreshTargetList()
	{
		if (!g_app.targetList) return;
		ListView_DeleteAllItems(g_app.targetList);
		g_app.targetSourceRows.clear();
		g_app.physicalBrowserRows.clear();
		g_app.targetListReadOnly = false;
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
			if (TargetTreeItem* item = SelectedTargetTreeItem(); IsPhysicalTreeFolder(item))
			{
				g_app.targetListReadOnly = true;
				SetTargetListHeader(L"当前目录内容（只读）");
				RefreshPhysicalFolderContents(item->physicalPath);
				UpdateTargetActionButtons();
				return;
			}
			SetTargetListHeader(L"当前分组中的目标项");
			const std::wstring folder = CurrentVirtualFolder();
			for (size_t i = 0; i < g_app.cmakeTarget.sources.size(); ++i)
			{
				const TargetSourceEntry& source = g_app.cmakeTarget.sources[i];
				if (!SameVirtualFolder(source.virtualFolder, folder)) continue;
				g_app.targetSourceRows.push_back(i);
				add(static_cast<int>(g_app.targetSourceRows.size() - 1), ProjectRelativeDisplayPath(source.path, source.isFolder));
			}
		}
		else
		{
			SetTargetListHeader(L"项目根目录相对路径 / 宏");
			const std::vector<std::wstring>* values = category == TargetIncludes ? &g_app.cmakeTarget.includeDirectories :
				(category == TargetDefinitions ? &g_app.cmakeTarget.compileDefinitions : &g_app.cmakeTarget.linkDirectories);
			const bool isPath = category != TargetDefinitions;
			for (size_t i = 0; i < values->size(); ++i)
				add(static_cast<int>(i), isPath ? ProjectRelativeDisplayPath((*values)[i], true) : (*values)[i]);
		}
		// 最后一行始终作为新项输入行，双击即可使用 ListView 原位编辑器创建配置。
		add(ListView_GetItemCount(g_app.targetList), L"");
		UpdateTargetActionButtons();
	}

	void RefreshSourceConfigurationViews()
	{
		RefreshTargetTree();
		RefreshTargetList();
	}

	void LayoutTargetToolbar(bool showAddFile)
	{
		const int top = 107 + (g_app.configMenuButton ? CUSTOM_MENU_HEIGHT : 0);
		constexpr int iconWidth = 36;
		constexpr int iconGap = 6;
		// 工具栏固定右对齐；类别切换只改变“添加文件”的可见性，不重排其它按钮。
		constexpr int virtualFolderX = 549;
		constexpr int fileX = 591;
		constexpr int folderX = fileX + iconWidth + iconGap;
		constexpr int deleteX = folderX + iconWidth + iconGap;
		constexpr int moveUpX = deleteX + iconWidth + iconGap;
		constexpr int moveDownX = moveUpX + iconWidth + iconGap;
		if (g_app.targetAddFile)
		{
			ShowWindow(g_app.targetAddFile, showAddFile ? SW_SHOW : SW_HIDE);
			MoveWindow(g_app.targetAddFile, Ui(fileX), Ui(top), Ui(iconWidth), Ui(32), TRUE);
		}
		if (g_app.targetAddVirtualFolder)
		{
			ShowWindow(g_app.targetAddVirtualFolder, showAddFile ? SW_SHOW : SW_HIDE);
			MoveWindow(g_app.targetAddVirtualFolder, Ui(virtualFolderX), Ui(top), Ui(iconWidth), Ui(32), TRUE);
		}
		if (g_app.targetAddFolder) {
			ShowWindow(g_app.targetAddFolder, SW_SHOW);
			MoveWindow(g_app.targetAddFolder, Ui(folderX), Ui(top), Ui(iconWidth), Ui(32), TRUE);
		}
		if (g_app.targetDelete) {
			ShowWindow(g_app.targetDelete, SW_SHOW);
			MoveWindow(g_app.targetDelete, Ui(deleteX), Ui(top), Ui(iconWidth), Ui(32), TRUE);
		}
		if (g_app.targetMoveUp) {
			ShowWindow(g_app.targetMoveUp, SW_SHOW);
			MoveWindow(g_app.targetMoveUp, Ui(moveUpX), Ui(top), Ui(iconWidth), Ui(32), TRUE);
		}
		if (g_app.targetMoveDown) {
			ShowWindow(g_app.targetMoveDown, SW_SHOW);
			MoveWindow(g_app.targetMoveDown, Ui(moveDownX), Ui(top), Ui(iconWidth), Ui(32), TRUE);
		}
		if (g_app.targetSave) {
			ShowWindow(g_app.targetSave, SW_SHOW);
			MoveWindow(g_app.targetSave, Ui(805), Ui(top), Ui(90), Ui(32), TRUE);
		}
	}

	void UpdateTargetCategoryControls()
	{
		const int category = SelectedTargetCategory();
		const bool sources = category == TargetSources;
		if (g_app.targetTree) ShowWindow(g_app.targetTree, sources ? SW_SHOW : SW_HIDE);
		if (g_app.targetList)
		{
			const int left = sources ? 306 : 14;
			const int width = sources ? 589 : 881;
			const int top = 149 + (g_app.configMenuButton ? CUSTOM_MENU_HEIGHT : 0);
			MoveWindow(g_app.targetList, Ui(left), Ui(top), Ui(width), Ui(271), TRUE);
			ListView_SetColumnWidth(g_app.targetList, 0, Ui(width - 4));
			LVCOLUMNW column{};
			column.mask = LVCF_TEXT;
			column.pszText = const_cast<wchar_t*>(sources ? L"当前分组中的目标项" : L"项目根目录相对路径 / 宏");
			ListView_SetColumn(g_app.targetList, 0, &column);
		}
		if (g_app.targetAddFolder)
		{
			const bool definitions = category == TargetDefinitions;
			SetWindowTextW(g_app.targetAddFolder, definitions ? L"添加编译宏" : (sources ? L"添加源目录" : L"添加目录"));
			UpdateToolTip(g_app.targetAddFolder, definitions ? L"添加编译宏" :
				(sources ? L"递归添加源目录" : L"添加目录"));
		}
		// 不支持文件的类别不显示“添加文件”和“新建虚拟文件夹”。
		LayoutTargetToolbar(sources);
		if (sources) RefreshSourceConfigurationViews();
		else RefreshTargetList();
		RedrawWindow(g_app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}

	void SwitchPage()
	{
		const bool targetPage = g_app.pageTab && TabCtrl_GetCurSel(g_app.pageTab) == 1;
		for (HWND control : g_app.toolPageControls) ShowWindow(control, targetPage ? SW_HIDE : SW_SHOW);
		for (HWND control : g_app.targetPageControls) ShowWindow(control, targetPage ? SW_SHOW : SW_HIDE);
		if (targetPage) UpdateTargetCategoryControls();
		// 透明标签隐藏时，父窗口也必须重绘，否则可能残留“配置类别”等旧文本。
		RedrawWindow(g_app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
		// 关闭系统 Explorer 主题，让控件颜色由本窗口的 GDI 主题消息统一控制。
		SetWindowTheme(control, L"", L"");
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	// uxtheme 的深色控件接口在 Windows 10/11 中以内部导出形式提供。
	// 动态查询使旧系统自动回退到常规控件主题，不影响程序启动。
	void EnableNativeDarkControls()
	{
		using SetPreferredAppModeFn = int (WINAPI*)(int);
		using FlushMenuThemesFn = void (WINAPI*)();
		HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
		if (!theme) return;
		auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
			GetProcAddress(theme, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(135))));
		if (setPreferredAppMode) setPreferredAppMode(2); // ForceDark
		auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
			GetProcAddress(theme, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(136))));
		if (flushMenuThemes) flushMenuThemes();
	}

	void EnableNativeDarkModeForWindow(HWND control)
	{
		using AllowDarkModeForWindowFn = BOOL (WINAPI*)(HWND, BOOL);
		HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
		if (!theme || !control) return;
		auto allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
			GetProcAddress(theme, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(133))));
		if (allowDarkModeForWindow) allowDarkModeForWindow(control, TRUE);
	}

	void ApplyDarkListView(HWND control, HFONT font)
	{
		if (!control) return;
		LONG_PTR style = GetWindowLongPtrW(control, GWL_EXSTYLE);
		style &= ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE);
		SetWindowLongPtrW(control, GWL_EXSTYLE, style);
		SetWindowPos(control, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		EnableNativeDarkModeForWindow(control);
		SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	void ApplyDarkTreeView(HWND control, HFONT font)
	{
		if (!control) return;
		LONG_PTR style = GetWindowLongPtrW(control, GWL_EXSTYLE);
		style &= ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE);
		SetWindowLongPtrW(control, GWL_EXSTYLE, style);
		SetWindowPos(control, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		EnableNativeDarkModeForWindow(control);
		SetWindowTheme(control, L"DarkMode_Explorer", nullptr);
		TreeView_SetBkColor(control, kThemeWindow);
		TreeView_SetTextColor(control, kThemeText);
		TreeView_SetLineColor(control, kThemeBorder);
		SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
	}

	LRESULT CALLBACK DarkControlSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR, DWORD_PTR)
	{
		if (message == WM_MOUSEMOVE)
		{
			if (ToolTipText(hwnd))
			{
				TRACKMOUSEEVENT tracking{};
				tracking.cbSize = sizeof(tracking);
				tracking.dwFlags = TME_LEAVE;
				tracking.hwndTrack = hwnd;
				TrackMouseEvent(&tracking);
				BeginToolTip(hwnd);
			}
		}
		else if (message == WM_MOUSELEAVE)
		{
			if (g_app.hoverControl == hwnd)
				HideToolTip();
		}
		else if (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN)
			HideToolTip();
		if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORSTATIC)
		{
			HDC dc = reinterpret_cast<HDC>(wParam);
			SetTextColor(dc, kThemeText);
			SetBkColor(dc, kThemeControl);
			return reinterpret_cast<LRESULT>(g_app.controlBrush ? g_app.controlBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
		if (message == WM_ERASEBKGND)
		{
			RECT client{};
			GetClientRect(hwnd, &client);
			FillRect(reinterpret_cast<HDC>(wParam), &client,
				g_app.controlBrush ? g_app.controlBrush : GetSysColorBrush(COLOR_WINDOW));
			return 1;
		}
		if (message == WM_NCPAINT || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE)
		{
			if (message != WM_NCPAINT)
				RedrawWindow(hwnd, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
			if (message == WM_NCPAINT)
			{
				HDC dc = GetWindowDC(hwnd);
				if (dc)
				{
					RECT rect{};
					GetWindowRect(hwnd, &rect);
					OffsetRect(&rect, -rect.left, -rect.top);
					const bool focused = GetFocus() == hwnd;
					const bool enabled = IsWindowEnabled(hwnd) != FALSE;
					HBRUSH brush = CreateSolidBrush(enabled ? (focused ? kThemeAccent : kThemeBorder) : kThemePanel);
					FrameRect(dc, &rect, brush);
					DeleteObject(brush);
					ReleaseDC(hwnd, dc);
				}
			}
		}
		return result;
	}

	void DrawDarkComboSurface(HWND hwnd, HDC dc)
	{
		RECT rect{};
		GetClientRect(hwnd, &rect);
		HBRUSH brush = CreateSolidBrush(kThemeControl);
		FillRect(dc, &rect, brush);
		DeleteObject(brush);
		const bool focused = GetFocus() == hwnd;
		HBRUSH borderBrush = CreateSolidBrush(focused ? kThemeAccent : kThemeBorder);
		FrameRect(dc, &rect, borderBrush);
		DeleteObject(borderBrush);
		RECT arrow = rect;
		arrow.left = arrow.right - Ui(27);
		HBRUSH arrowBrush = CreateSolidBrush(kThemePanel);
		FillRect(dc, &arrow, arrowBrush);
		DeleteObject(arrowBrush);
		POINT triangle[] = {
			{(arrow.left + arrow.right) / 2 - Ui(5), (arrow.top + arrow.bottom) / 2 - Ui(2)},
			{(arrow.left + arrow.right) / 2 + Ui(5), (arrow.top + arrow.bottom) / 2 - Ui(2)},
			{(arrow.left + arrow.right) / 2, (arrow.top + arrow.bottom) / 2 + Ui(4)}
		};
		HBRUSH triangleBrush = CreateSolidBrush(kThemeText);
		HGDIOBJ oldBrush = SelectObject(dc, triangleBrush);
		HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
		Polygon(dc, triangle, static_cast<int>(ARRAY_SIZE(triangle)));
		SelectObject(dc, oldPen);
		SelectObject(dc, oldBrush);
		DeleteObject(triangleBrush);
		wchar_t text[256]{};
		GetWindowTextW(hwnd, text, static_cast<int>(ARRAY_SIZE(text)));
		RECT textRect = rect;
		textRect.left += Ui(8);
		textRect.right = arrow.left - Ui(4);
		DrawThemedText(dc, text, textRect, kThemeText, ThemedTextAlign::Left, false, true);
	}

	LRESULT CALLBACK DarkComboSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR, DWORD_PTR)
	{
		if (message == WM_PAINT)
		{
			PAINTSTRUCT paint{};
			HDC dc = BeginPaint(hwnd, &paint);
			DrawDarkComboSurface(hwnd, dc);
			EndPaint(hwnd, &paint);
			return 0;
		}
		if (message == WM_ERASEBKGND)
			return 1;
		if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SETCURSEL)
			InvalidateRect(hwnd, nullptr, TRUE);
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}

	void ApplyDarkControl(HWND control, HFONT font, bool removeClientEdge = false)
	{
		if (!control)
			return;
		ApplyModernTheme(control, font);
		if (removeClientEdge)
		{
			LONG_PTR style = GetWindowLongPtrW(control, GWL_EXSTYLE);
			style &= ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE);
			SetWindowLongPtrW(control, GWL_EXSTYLE, style);
			SetWindowPos(control, nullptr, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		}
		wchar_t className[32]{};
		GetClassNameW(control, className, static_cast<int>(ARRAY_SIZE(className)));
		SetWindowSubclass(control, _wcsicmp(className, WC_COMBOBOXW) == 0 ? DarkComboSubclass : DarkControlSubclass, 1, 0);
	}

	void DrawDarkTabSurface(HWND hwnd, HDC dc)
	{
		RECT client{};
		GetClientRect(hwnd, &client);
		FillRect(dc, &client, g_app.panelBrush ? g_app.panelBrush : CreateSolidBrush(kThemePanel));
		const int count = TabCtrl_GetItemCount(hwnd);
		for (int index = 0; index < count; ++index)
		{
			RECT rect{};
			TabCtrl_GetItemRect(hwnd, index, &rect);
			const bool selected = index == TabCtrl_GetCurSel(hwnd);
			HBRUSH brush = CreateSolidBrush(selected ? kThemeControl : kThemePanel);
			FillRect(dc, &rect, brush);
			DeleteObject(brush);
			HPEN pen = CreatePen(PS_SOLID, 1, selected ? kThemeAccent : kThemeBorder);
			HGDIOBJ oldPen = SelectObject(dc, pen);
			HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
			Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
			wchar_t text[128]{};
			TCITEMW item{};
			item.mask = TCIF_TEXT;
			item.pszText = text;
			item.cchTextMax = static_cast<int>(ARRAY_SIZE(text));
			TabCtrl_GetItem(hwnd, index, &item);
			RECT textRect = rect;
			DrawThemedText(dc, text, textRect, selected ? kThemeText : kThemeMutedText, ThemedTextAlign::Center);
		}
	}

	LRESULT CALLBACK DarkTabSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR, DWORD_PTR)
	{
		if (message == WM_PAINT)
		{
			PAINTSTRUCT paint{};
			HDC dc = BeginPaint(hwnd, &paint);
			DrawDarkTabSurface(hwnd, dc);
			EndPaint(hwnd, &paint);
			return 0;
		}
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}

	void DrawDarkHeaderSurface(HWND hwnd, HDC dc)
	{
		RECT client{};
		GetClientRect(hwnd, &client);
		HBRUSH brush = CreateSolidBrush(kThemePanel);
		FillRect(dc, &client, brush);
		DeleteObject(brush);
		const int count = Header_GetItemCount(hwnd);
		for (int index = 0; index < count; ++index)
		{
			RECT rect{};
			Header_GetItemRect(hwnd, index, &rect);
			HPEN pen = CreatePen(PS_SOLID, 1, kThemeBorder);
			HGDIOBJ oldPen = SelectObject(dc, pen);
			HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
			Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
			SelectObject(dc, oldBrush);
			SelectObject(dc, oldPen);
			DeleteObject(pen);
			wchar_t text[256]{};
			HDITEMW item{};
			item.mask = HDI_TEXT;
			item.pszText = text;
			item.cchTextMax = static_cast<int>(ARRAY_SIZE(text));
			Header_GetItem(hwnd, index, &item);
			RECT textRect = rect;
			textRect.left += Ui(8);
			DrawThemedText(dc, text, textRect, kThemeText, ThemedTextAlign::Left, false, true);
		}
	}

	LRESULT CALLBACK DarkHeaderSubclass(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
		UINT_PTR, DWORD_PTR)
	{
		if (message == WM_PAINT)
		{
			PAINTSTRUCT paint{};
			HDC dc = BeginPaint(hwnd, &paint);
			DrawDarkHeaderSurface(hwnd, dc);
			EndPaint(hwnd, &paint);
			return 0;
		}
		if (message == WM_ERASEBKGND)
			return 1;
		return DefSubclassProc(hwnd, message, wParam, lParam);
	}

	void AppendDarkMenuItem(HMENU menu, UINT flags, UINT_PTR id, const wchar_t* text)
	{
		AppendMenuW(menu, flags | MF_OWNERDRAW, id, reinterpret_cast<LPCWSTR>(text));
	}

	void AppendDarkMenuSeparator(HMENU menu)
	{
		// 系统 MF_SEPARATOR 会使用系统白色主题，因此也作为 owner-draw 项处理。
		AppendMenuW(menu, MF_OWNERDRAW, 0, reinterpret_cast<LPCWSTR>(L""));
	}

	void ApplyDarkMenu(HMENU menu)
	{
		if (!menu)
			return;
		MENUINFO info{};
		info.cbSize = sizeof(info);
		info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
		info.hbrBack = g_app.panelBrush;
		SetMenuInfo(menu, &info);
	}

	void ShiftDirectChildControls(HWND parent, int offsetY)
	{
		for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
		{
			RECT rect{};
			GetWindowRect(child, &rect);
			MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&rect), 2);
			SetWindowPos(child, nullptr, rect.left, rect.top + offsetY,
				rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	void ShowCustomPopupMenu(HWND button, HMENU popup)
	{
		if (!button || !popup || !g_app.window)
			return;
		RECT rect{};
		GetWindowRect(button, &rect);
		const UINT command = TrackPopupMenuEx(popup,
			TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD,
			rect.left, rect.bottom, g_app.window, nullptr);
		if (command != 0)
			SendMessageW(g_app.window, WM_COMMAND, MAKEWPARAM(command, 0), 0);
	}

	bool DrawDarkMenuItem(const DRAWITEMSTRUCT* item)
	{
		if (!item || item->CtlType != ODT_MENU)
			return false;
		const bool selected = (item->itemState & ODS_SELECTED) != 0;
		const bool disabled = (item->itemState & ODS_DISABLED) != 0;
		HBRUSH brush = CreateSolidBrush(selected ? kThemeSelection : kThemePanel);
		FillRect(item->hDC, &item->rcItem, brush);
		DeleteObject(brush);
		if (item->itemID == 0)
		{
			const int y = (item->rcItem.top + item->rcItem.bottom) / 2;
			HPEN pen = CreatePen(PS_SOLID, 1, kThemeBorder);
			HGDIOBJ oldPen = SelectObject(item->hDC, pen);
			MoveToEx(item->hDC, item->rcItem.left + Ui(8), y, nullptr);
			LineTo(item->hDC, item->rcItem.right - Ui(8), y);
			SelectObject(item->hDC, oldPen);
			DeleteObject(pen);
			return true;
		}
		if (item->itemData)
		{
			RECT textRect = item->rcItem;
			textRect.left += Ui(10);
			DrawThemedText(item->hDC, reinterpret_cast<const wchar_t*>(item->itemData), textRect,
				disabled ? kThemeMutedText : kThemeText, ThemedTextAlign::Left, false, true);
		}
		if ((item->itemState & ODS_FOCUS) != 0)
		{
			RECT focus = item->rcItem;
			InflateRect(&focus, -1, -1);
			DrawFocusRect(item->hDC, &focus);
		}
		return true;
	}

	bool MeasureDarkMenuItem(MEASUREITEMSTRUCT* item)
	{
		if (!item || item->CtlType != ODT_MENU)
			return false;
		if (item->itemID == 0)
		{
			item->itemWidth = Ui(180);
			item->itemHeight = Ui(10);
			return true;
		}
		if (!item->itemData)
			return false;
		HDC dc = GetDC(g_app.window);
		HFONT oldFont = dc ? reinterpret_cast<HFONT>(SelectObject(dc, g_app.font)) : nullptr;
		RECT rect{0, 0, 0, 0};
		const wchar_t* text = reinterpret_cast<const wchar_t*>(item->itemData);
		if (dc) DrawTextW(dc, text, -1, &rect, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
		if (dc)
		{
			SelectObject(dc, oldFont);
			ReleaseDC(g_app.window, dc);
		}
		item->itemWidth = static_cast<UINT>(rect.right - rect.left + Ui(28));
		item->itemHeight = static_cast<UINT>(Ui(30));
		return true;
	}

	bool IsTargetToolbarButton(UINT id)
	{
		return id == ID_TARGET_ADD_VIRTUAL_FOLDER || id == ID_TARGET_ADD_FILE || id == ID_TARGET_ADD_FOLDER || id == ID_TARGET_DELETE ||
			id == ID_TARGET_MOVE_UP || id == ID_TARGET_MOVE_DOWN;
	}

	bool IsOwnerDrawButton(UINT id)
	{
		return IsTargetToolbarButton(id) || (id >= ID_BROWSE_BASE && id < ID_BROWSE_BASE + 8) ||
			id == ID_CLEAR || id == ID_SAVE_EXAMPLE || id == ID_SAVE_SETTINGS || id == ID_TARGET_SAVE ||
			id == ID_CONFIG_MENU_BUTTON || id == ID_OTHER_MENU_BUTTON || id == ID_ABOUT_CLOSE;
	}

	void DrawToolbarLine(HDC dc, COLORREF color, int width, int x1, int y1, int x2, int y2)
	{
		HPEN pen = CreatePen(PS_SOLID, width, color);
		HGDIOBJ previous = SelectObject(dc, pen);
		MoveToEx(dc, x1, y1, nullptr);
		LineTo(dc, x2, y2);
		SelectObject(dc, previous);
		DeleteObject(pen);
	}

	void DrawToolbarPlus(HDC dc, COLORREF color, int centerX, int centerY, int radius, int width)
	{
		DrawToolbarLine(dc, color, width, centerX - radius, centerY, centerX + radius, centerY);
		DrawToolbarLine(dc, color, width, centerX, centerY - radius, centerX, centerY + radius);
	}

	void DrawToolbarArrow(HDC dc, COLORREF color, int centerX, int centerY, bool up)
	{
		const int direction = up ? -1 : 1;
		POINT points[] = {
			{centerX, centerY + direction * 8},
			{centerX - 8, centerY},
			{centerX - 3, centerY},
			{centerX - 3, centerY - direction * 8},
			{centerX + 3, centerY - direction * 8},
			{centerX + 3, centerY},
			{centerX + 8, centerY}
		};
		HBRUSH brush = CreateSolidBrush(color);
		HGDIOBJ previousBrush = SelectObject(dc, brush);
		HGDIOBJ previousPen = SelectObject(dc, GetStockObject(NULL_PEN));
		Polygon(dc, points, static_cast<int>(ARRAY_SIZE(points)));
		SelectObject(dc, previousPen);
		SelectObject(dc, previousBrush);
		DeleteObject(brush);
	}

	void DrawToolbarDocument(HDC dc, COLORREF color, int centerX, int centerY)
	{
		const int left = centerX - 8;
		const int top = centerY - 9;
		const int right = centerX + 4;
		const int bottom = centerY + 8;
		DrawToolbarLine(dc, color, 2, left, top, right - 4, top);
		DrawToolbarLine(dc, color, 2, right - 4, top, right, top + 4);
		DrawToolbarLine(dc, color, 2, right, top + 4, right, bottom);
		DrawToolbarLine(dc, color, 2, right, bottom, left, bottom);
		DrawToolbarLine(dc, color, 2, left, bottom, left, top);
		DrawToolbarLine(dc, color, 1, right - 4, top, right - 4, top + 4);
		DrawToolbarLine(dc, color, 1, right - 4, top + 4, right, top + 4);
		DrawToolbarPlus(dc, color, centerX + 6, centerY + 6, 3, 2);
	}

	void DrawToolbarFolder(HDC dc, COLORREF color, int centerX, int centerY)
	{
		const int left = centerX - 9;
		const int top = centerY - 5;
		const int right = centerX + 5;
		const int bottom = centerY + 7;
		DrawToolbarLine(dc, color, 2, left, top, left + 5, top);
		DrawToolbarLine(dc, color, 2, left + 5, top, left + 7, top + 3);
		DrawToolbarLine(dc, color, 2, left + 7, top + 3, right, top + 3);
		DrawToolbarLine(dc, color, 2, right, top + 3, right, bottom);
		DrawToolbarLine(dc, color, 2, right, bottom, left, bottom);
		DrawToolbarLine(dc, color, 2, left, bottom, left, top);
		DrawToolbarPlus(dc, color, centerX + 7, centerY + 6, 3, 2);
	}

	void DrawToolbarMacro(HDC dc, COLORREF color, int centerX, int centerY)
	{
		DrawToolbarLine(dc, color, 2, centerX - 8, centerY - 7, centerX - 9, centerY + 7);
		DrawToolbarLine(dc, color, 2, centerX - 2, centerY - 7, centerX - 3, centerY + 7);
		DrawToolbarLine(dc, color, 2, centerX - 11, centerY - 3, centerX + 1, centerY - 4);
		DrawToolbarLine(dc, color, 2, centerX - 11, centerY + 3, centerX + 1, centerY + 2);
		DrawToolbarPlus(dc, color, centerX + 7, centerY + 5, 3, 2);
	}

	bool DrawTargetToolbarButton(const DRAWITEMSTRUCT* item)
	{
		if (!item || !IsOwnerDrawButton(item->CtlID))
			return false;
		const RECT& rect = item->rcItem;
		const bool disabled = (item->itemState & ODS_DISABLED) != 0;
		const bool pressed = (item->itemState & ODS_SELECTED) != 0;
		const bool hot = (item->itemState & ODS_HOTLIGHT) != 0;
		const bool menuButton = item->CtlID == ID_CONFIG_MENU_BUTTON || item->CtlID == ID_OTHER_MENU_BUTTON;
		const COLORREF background = menuButton ? (pressed ? kThemeSelection : (hot ? kThemeHover : kThemeWindow)) :
			(disabled ? kThemePanel : (pressed ? kThemeSelection : (hot ? kThemeHover : kThemeControl)));
		const bool primary = item->CtlID == ID_SAVE_EXAMPLE || item->CtlID == ID_SAVE_SETTINGS || item->CtlID == ID_TARGET_SAVE;
		const COLORREF border = primary ? kThemeAccent : (pressed ? kThemeAccent : (hot ? RGB(90, 150, 200) : kThemeBorder));
		const COLORREF icon = disabled ? kThemeMutedText : kThemeText;
		HBRUSH brush = CreateSolidBrush(background);
		FillRect(item->hDC, &rect, brush);
		DeleteObject(brush);
		if (!menuButton)
		{
			HPEN pen = CreatePen(PS_SOLID, 1, border);
			HGDIOBJ previousPen = SelectObject(item->hDC, pen);
			HGDIOBJ previousBrush = SelectObject(item->hDC, GetStockObject(HOLLOW_BRUSH));
			Rectangle(item->hDC, rect.left, rect.top, rect.right, rect.bottom);
			SelectObject(item->hDC, previousBrush);
			SelectObject(item->hDC, previousPen);
			DeleteObject(pen);
		}

		const int centerX = (rect.left + rect.right) / 2 + (pressed ? 1 : 0);
		const int centerY = (rect.top + rect.bottom) / 2 + (pressed ? 1 : 0);
		switch (item->CtlID)
		{
		case ID_TARGET_ADD_VIRTUAL_FOLDER: DrawToolbarFolder(item->hDC, kThemeAccent, centerX, centerY); break;
		case ID_TARGET_ADD_FILE: DrawToolbarDocument(item->hDC, icon, centerX, centerY); break;
		case ID_TARGET_ADD_FOLDER:
			if (SelectedTargetCategory() == TargetDefinitions) DrawToolbarMacro(item->hDC, icon, centerX, centerY);
			else DrawToolbarFolder(item->hDC, icon, centerX, centerY);
			break;
		case ID_TARGET_DELETE:
			DrawToolbarLine(item->hDC, icon, 2, centerX - 7, centerY - 7, centerX + 7, centerY + 7);
			DrawToolbarLine(item->hDC, icon, 2, centerX + 7, centerY - 7, centerX - 7, centerY + 7);
			break;
		case ID_TARGET_MOVE_UP: DrawToolbarArrow(item->hDC, icon, centerX, centerY, true); break;
		case ID_TARGET_MOVE_DOWN: DrawToolbarArrow(item->hDC, icon, centerX, centerY, false); break;
		default:
		{
			wchar_t text[128]{};
			GetWindowTextW(item->hwndItem, text, static_cast<int>(ARRAY_SIZE(text)));
			RECT textRect = rect;
			DrawThemedText(item->hDC, text, textRect, disabled ? kThemeMutedText : kThemeText, ThemedTextAlign::Center, false, true);
			break;
		}
		}
		if ((item->itemState & ODS_FOCUS) != 0)
		{
			RECT focus = rect;
			InflateRect(&focus, -3, -3);
			DrawFocusRect(item->hDC, &focus);
		}
		return true;
	}

	bool DrawPageTabItem(const DRAWITEMSTRUCT* item)
	{
		if (!item || item->CtlID != ID_PAGE_TAB)
			return false;
		const int index = static_cast<int>(item->itemID);
		const bool selected = index == TabCtrl_GetCurSel(g_app.pageTab);
		const RECT& rect = item->rcItem;
		HBRUSH brush = CreateSolidBrush(selected ? kThemeControl : kThemePanel);
		FillRect(item->hDC, &rect, brush);
		DeleteObject(brush);
		HPEN pen = CreatePen(PS_SOLID, 1, selected ? kThemeAccent : kThemeBorder);
		HGDIOBJ oldPen = SelectObject(item->hDC, pen);
		HGDIOBJ oldBrush = SelectObject(item->hDC, GetStockObject(HOLLOW_BRUSH));
		Rectangle(item->hDC, rect.left, rect.top, rect.right, rect.bottom);
		SelectObject(item->hDC, oldBrush);
		SelectObject(item->hDC, oldPen);
		DeleteObject(pen);
		wchar_t text[128]{};
		TCITEMW tab{};
		tab.mask = TCIF_TEXT;
		tab.pszText = text;
		tab.cchTextMax = static_cast<int>(ARRAY_SIZE(text));
		TabCtrl_GetItem(g_app.pageTab, index, &tab);
		RECT textRect = rect;
		DrawThemedText(item->hDC, text, textRect, selected ? kThemeText : kThemeMutedText, ThemedTextAlign::Center, false, true);
		return true;
	}

	LRESULT DrawPageTabCustom(const NMCUSTOMDRAW* custom)
	{
		if (!custom)
			return CDRF_DODEFAULT;
		if (custom->dwDrawStage == CDDS_PREPAINT)
			return CDRF_NOTIFYITEMDRAW;
		if (custom->dwDrawStage != CDDS_ITEMPREPAINT)
			return CDRF_DODEFAULT;
		const int index = static_cast<int>(custom->dwItemSpec);
		RECT rect{};
		TabCtrl_GetItemRect(g_app.pageTab, index, &rect);
		const bool selected = index == TabCtrl_GetCurSel(g_app.pageTab);
		HBRUSH brush = CreateSolidBrush(selected ? kThemeControl : kThemePanel);
		FillRect(custom->hdc, &rect, brush);
		DeleteObject(brush);
		HPEN pen = CreatePen(PS_SOLID, 1, selected ? kThemeAccent : kThemeBorder);
		HGDIOBJ oldPen = SelectObject(custom->hdc, pen);
		HGDIOBJ oldBrush = SelectObject(custom->hdc, GetStockObject(HOLLOW_BRUSH));
		Rectangle(custom->hdc, rect.left, rect.top, rect.right, rect.bottom);
		SelectObject(custom->hdc, oldBrush);
		SelectObject(custom->hdc, oldPen);
		DeleteObject(pen);
		wchar_t text[128]{};
		TCITEMW item{};
		item.mask = TCIF_TEXT;
		item.pszText = text;
		item.cchTextMax = static_cast<int>(ARRAY_SIZE(text));
		TabCtrl_GetItem(g_app.pageTab, index, &item);
		DrawThemedText(custom->hdc, text, rect, selected ? kThemeText : kThemeMutedText, ThemedTextAlign::Center, false, true);
		return CDRF_SKIPDEFAULT;
	}

	LRESULT DrawTargetListCustom(const NMCUSTOMDRAW* custom)
	{
		if (!custom)
			return CDRF_DODEFAULT;
		if (custom->dwDrawStage == CDDS_PREPAINT)
			return CDRF_NOTIFYITEMDRAW;
		if (custom->dwDrawStage != CDDS_ITEMPREPAINT)
			return CDRF_DODEFAULT;
		NMLVCUSTOMDRAW* list = reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMCUSTOMDRAW*>(custom));
		const int row = static_cast<int>(custom->dwItemSpec);
		const bool selected = (ListView_GetItemState(g_app.targetList, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
		const HWND focus = GetFocus();
		const bool active = focus == g_app.targetList || (focus && IsChild(g_app.targetList, focus));
		list->clrText = kThemeText;
		// 仅当前焦点所在的一侧使用蓝色；另一侧保留低对比深色选中，避免两边同时“高亮”。
		list->clrTextBk = selected ? (active ? kThemeSelection : kThemeControl) : kThemeWindow;
		return CDRF_NEWFONT;
	}

	LRESULT DrawTargetTreeCustom(const NMCUSTOMDRAW* custom)
	{
		if (!custom)
			return CDRF_DODEFAULT;
		if (custom->dwDrawStage == CDDS_PREPAINT)
			return CDRF_NOTIFYITEMDRAW;
		if (custom->dwDrawStage != CDDS_ITEMPREPAINT)
			return CDRF_DODEFAULT;
		NMTVCUSTOMDRAW* tree = reinterpret_cast<NMTVCUSTOMDRAW*>(const_cast<NMCUSTOMDRAW*>(custom));
		const bool selected = (custom->uItemState & CDIS_SELECTED) != 0;
		const TargetTreeItem* item = TargetTreeItemFromHandle(reinterpret_cast<HTREEITEM>(custom->dwItemSpec));
		const HWND focus = GetFocus();
		const bool active = focus == g_app.targetTree || (focus && IsChild(g_app.targetTree, focus));
		COLORREF text = kThemeText;
		if (item)
		{
			switch (item->kind)
			{
			case TargetTreeItemKind::VirtualFolder: text = selected ? RGB(205, 235, 255) : RGB(105, 185, 245); break;
			case TargetTreeItemKind::PhysicalSourceFolder: text = selected ? RGB(255, 226, 148) : RGB(231, 191, 84); break;
			case TargetTreeItemKind::PhysicalFolder: text = selected ? RGB(234, 218, 170) : RGB(184, 166, 116); break;
			case TargetTreeItemKind::Root: text = kThemeMutedText; break;
			default: break;
			}
		}
		tree->clrText = text;
		tree->clrTextBk = selected ? (active ? kThemeSelection : kThemeControl) : kThemeWindow;
		return CDRF_NEWFONT;
	}

	LRESULT DrawHeaderCustom(const NMCUSTOMDRAW* custom)
	{
		if (!custom)
			return CDRF_DODEFAULT;
		if (custom->dwDrawStage == CDDS_PREPAINT)
			return CDRF_NOTIFYITEMDRAW;
		if (custom->dwDrawStage != CDDS_ITEMPREPAINT)
			return CDRF_DODEFAULT;
		RECT rect = custom->rc;
		HBRUSH brush = CreateSolidBrush(kThemePanel);
		FillRect(custom->hdc, &rect, brush);
		DeleteObject(brush);
		HPEN pen = CreatePen(PS_SOLID, 1, kThemeBorder);
		HGDIOBJ oldPen = SelectObject(custom->hdc, pen);
		HGDIOBJ oldBrush = SelectObject(custom->hdc, GetStockObject(HOLLOW_BRUSH));
		Rectangle(custom->hdc, rect.left, rect.top, rect.right, rect.bottom);
		SelectObject(custom->hdc, oldBrush);
		SelectObject(custom->hdc, oldPen);
		DeleteObject(pen);
		HWND header = ListView_GetHeader(g_app.targetList);
		wchar_t text[256]{};
		HDITEMW item{};
		item.mask = HDI_TEXT;
		item.pszText = text;
		item.cchTextMax = static_cast<int>(ARRAY_SIZE(text));
		Header_GetItem(header, static_cast<int>(custom->dwItemSpec), &item);
		RECT textRect = rect;
		textRect.left += Ui(8);
		DrawThemedText(custom->hdc, text, textRect, kThemeText, ThemedTextAlign::Left, false, true);
		return CDRF_SKIPDEFAULT;
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

	bool Utf8ToWide(const std::string &value, std::wstring &result)
	{
		if (value.empty())
		{
			result.clear();
			return true;
		}
		const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), nullptr, 0);
		if (length <= 0)
			return false;
		result.resize(static_cast<size_t>(length));
		return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
			static_cast<int>(value.size()), result.data(), length) == length;
	}

	bool ExtractJsonString(const std::string &json, const char *key, std::string &value)
	{
		const std::string marker = std::string("\"") + key + "\"";
		const size_t keyPosition = json.find(marker);
		if (keyPosition == std::string::npos)
			return false;
		const size_t colon = json.find(':', keyPosition + marker.size());
		if (colon == std::string::npos)
			return false;
		const size_t quote = json.find('"', colon + 1);
		if (quote == std::string::npos)
			return false;
		value.clear();
		bool escaped = false;
		for (size_t i = quote + 1; i < json.size(); ++i)
		{
			const char current = json[i];
			if (escaped)
			{
				if (current == '"' || current == '\\' || current == '/') value.push_back(current);
				else if (current == 'n') value.push_back('\n');
				else if (current == 'r') value.push_back('\r');
				else if (current == 't') value.push_back('\t');
				else value.push_back(current);
				escaped = false;
				continue;
			}
			if (current == '\\') { escaped = true; continue; }
			if (current == '"') return true;
			value.push_back(current);
		}
		return false;
	}

	struct VersionParts
	{
		int value[3]{};
	};

	bool ParseVersion(const std::wstring &text, VersionParts &version)
	{
		version = {};
		size_t position = 0;
		while (position < text.size() && !iswdigit(text[position])) ++position;
		int component = 0;
		while (position < text.size() && component < 3)
		{
			while (position < text.size() && !iswdigit(text[position])) ++position;
			if (position == text.size()) break;
			int number = 0;
			while (position < text.size() && iswdigit(text[position]))
			{
				const int digit = text[position++] - L'0';
				if (number <= (std::numeric_limits<int>::max() - digit) / 10)
					number = number * 10 + digit;
				else
					number = std::numeric_limits<int>::max();
			}
			version.value[component++] = number;
		}
		return component > 0;
	}

	int CompareVersions(const VersionParts &left, const VersionParts &right)
	{
		for (size_t i = 0; i < ARRAY_SIZE(left.value); ++i)
			if (left.value[i] != right.value[i]) return left.value[i] < right.value[i] ? -1 : 1;
		return 0;
	}

	bool FetchLatestRelease(LatestRelease &release)
	{
		HINTERNET session = WinHttpOpen(L"PathConfigurator/1.0.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
			WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		HINTERNET connection = nullptr;
		HINTERNET request = nullptr;
		std::string response;
		bool requestOk = false;
		do
		{
			if (!session) break;
			// 更新检查不能影响配置器的交互。网络不可用或代理无响应时快速放弃，
			// 下次启动再检查即可。
			WinHttpSetTimeouts(session, 1500, 1500, 2500, 2500);
			connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
			if (!connection) break;
			request = WinHttpOpenRequest(connection, L"GET", L"/repos/Name-CK/PathConfigurator/releases/latest",
				nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
			if (!request) break;
			const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nUser-Agent: PathConfigurator/1.0.0\r\n";
			if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) break;
			if (!WinHttpReceiveResponse(request, nullptr)) break;
			DWORD statusCode = 0;
			DWORD statusLength = sizeof(statusCode);
			if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLength, WINHTTP_NO_HEADER_INDEX) || statusCode != 200) break;
			for (;;)
			{
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request, &available)) break;
				if (available == 0) { requestOk = true; break; }
				if (response.size() + available > 1024 * 1024) break;
				std::string chunk(available, '\0');
				DWORD received = 0;
				if (!WinHttpReadData(request, chunk.data(), available, &received)) break;
				response.append(chunk.data(), received);
			}
		} while (false);
		if (request) WinHttpCloseHandle(request);
		if (connection) WinHttpCloseHandle(connection);
		if (session) WinHttpCloseHandle(session);
		if (!requestOk) return false;

		std::string tag;
		std::string url;
		if (!ExtractJsonString(response, "tag_name", tag) || !Utf8ToWide(tag, release.tag)) return false;
		if (ExtractJsonString(response, "html_url", url) && !Utf8ToWide(url, release.url)) release.url.clear();
		if (release.url.empty()) release.url = kGitHubReleasePage;
		return true;
	}

	std::wstring GetUpdateCachePath()
	{
		const std::wstring settings = GetUserDefaultSettingsPath();
		const std::wstring directory = GetParentPath(settings);
		return directory.empty() ? std::wstring{} : JoinPath(directory, L"update-cache.dat");
	}

	bool WriteUpdateCache(const LatestRelease& release)
	{
		const std::wstring path = GetUpdateCachePath();
		if (path.empty()) return false;
		const std::wstring directory = GetParentPath(path);
		if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
			return false;
		const std::wstring document = L"PathConfiguratorUpdateCache1\n" + release.tag + L"\n" + release.url;
		const std::wstring temporary = path + L".tmp";
		HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		const DWORD byteCount = static_cast<DWORD>(document.size() * sizeof(wchar_t));
		DWORD written = 0;
		const bool ok = WriteFile(file, document.data(), byteCount, &written, nullptr) && written == byteCount;
		FlushFileBuffers(file);
		CloseHandle(file);
		if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
	}

	bool ReadUpdateCache(LatestRelease& release)
	{
		const std::wstring path = GetUpdateCachePath();
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 ||
			(size.QuadPart % sizeof(wchar_t)) != 0)
		{
			CloseHandle(file);
			return false;
		}
		std::wstring document(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)), L'\0');
		DWORD read = 0;
		const bool ok = ReadFile(file, document.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) &&
			read == static_cast<DWORD>(size.QuadPart);
		CloseHandle(file);
		if (!ok) return false;
		const std::wstring marker = L"PathConfiguratorUpdateCache1\n";
		if (document.rfind(marker, 0) != 0) return false;
		const size_t tagEnd = document.find(L'\n', marker.size());
		if (tagEnd == std::wstring::npos) return false;
		release.tag = document.substr(marker.size(), tagEnd - marker.size());
		release.url = document.substr(tagEnd + 1);
		return !release.tag.empty() && !release.url.empty();
	}

	int RunUpdateCacheCheck()
	{
		LatestRelease latest;
		return FetchLatestRelease(latest) && WriteUpdateCache(latest) ? 0 : 1;
	}

	std::wstring DisplayVersionTag(std::wstring tag);

	void ApplyCachedUpdate()
	{
		LatestRelease latest;
		VersionParts currentVersion{};
		VersionParts latestVersion{};
		if (!g_app.updateLink || !ReadUpdateCache(latest) ||
			!ParseVersion(kAppVersion, currentVersion) || !ParseVersion(latest.tag, latestVersion) ||
			CompareVersions(latestVersion, currentVersion) <= 0)
		{
			if (g_app.updateLink) ShowWindow(g_app.updateLink, SW_HIDE);
			return;
		}
		SetWindowTextW(g_app.updateLink, (L"有最新版 " + DisplayVersionTag(latest.tag)).c_str());
		g_app.updateUrl = std::move(latest.url);
		ShowWindow(g_app.updateLink, SW_SHOW);
	}

	bool LaunchUpdateCacheCheck(HANDLE& updateProcess)
	{
		updateProcess = nullptr;
		wchar_t modulePath[MAX_PATH * 4]{};
		if (!GetModuleFileNameW(nullptr, modulePath, static_cast<DWORD>(ARRAY_SIZE(modulePath))))
			return false;
		std::wstring command = L"\"" + std::wstring(modulePath) + L"\" --check-update-cache";
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(modulePath, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			nullptr, nullptr, &startup, &process))
			return false;
		CloseHandle(process.hThread);
		updateProcess = process.hProcess;
		return true;
	}

	void StartUpdateCheck(HWND owner)
	{
		if (!owner || g_app.updateCheckStarted) return;
		g_app.updateCheckStarted = true;
		if (LaunchUpdateCacheCheck(g_app.updateProcess))
			SetTimer(owner, ID_UPDATE_PROCESS_TIMER, 250, nullptr);
	}

	std::wstring DisplayVersionTag(std::wstring tag)
	{
		if (!tag.empty() && tag.front() != L'v' && tag.front() != L'V')
			tag.insert(tag.begin(), L'V');
		return tag;
	}

	void SetAboutUpdateState(AboutUpdateState state, const std::wstring& message, const std::wstring& url = L"")
	{
		g_about.updateState = state;
		g_about.updateUrl = url;
		if (g_about.status)
		{
			SetWindowTextW(g_about.status, message.c_str());
			InvalidateRect(g_about.status, nullptr, TRUE);
		}
	}

	void CompleteAboutUpdateCheck()
	{
		LatestRelease latest;
		VersionParts currentVersion{};
		VersionParts latestVersion{};
		if (!ReadUpdateCache(latest) || !ParseVersion(kAppVersion, currentVersion) || !ParseVersion(latest.tag, latestVersion))
		{
			SetAboutUpdateState(AboutUpdateState::Failed, L"检查更新失败");
			return;
		}
		const std::wstring latestTag = DisplayVersionTag(latest.tag);
		if (CompareVersions(latestVersion, currentVersion) <= 0)
		{
			SetAboutUpdateState(AboutUpdateState::Latest, L"当前已是最新版（" + latestTag + L"）");
			return;
		}
		SetAboutUpdateState(AboutUpdateState::Outdated,
			L"检测到新版 " + latestTag + L"（当前版本较旧：V" + kAppVersion + L"），点击此处打开发布页", latest.url);
	}

	LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
		case WM_CREATE:
		{
			g_about.window = hwnd;
			BOOL darkFrame = TRUE;
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkFrame, sizeof(darkFrame));
			HWND title = CreateWindowW(L"STATIC", L"STM32 项目配置器", WS_CHILD | WS_VISIBLE,
				Ui(18), Ui(16), Ui(420), Ui(30), hwnd, nullptr, nullptr, nullptr);
			HWND current = CreateWindowW(L"STATIC", (L"当前版本：V" + std::wstring(kAppVersion)).c_str(), WS_CHILD | WS_VISIBLE,
				Ui(18), Ui(55), Ui(420), Ui(24), hwnd, nullptr, nullptr, nullptr);
			HWND repository = CreateWindowW(L"STATIC", (L"仓库：" + std::wstring(kGitHubRepository)).c_str(),
				WS_CHILD | WS_VISIBLE | SS_NOTIFY, Ui(18), Ui(82), Ui(430), Ui(24), hwnd,
				reinterpret_cast<HMENU>(ID_ABOUT_REPOSITORY), nullptr, nullptr);
			g_about.status = CreateWindowW(L"STATIC", L"正在检查更新...", WS_CHILD | WS_VISIBLE | SS_NOTIFY,
				Ui(18), Ui(116), Ui(430), Ui(50), hwnd, reinterpret_cast<HMENU>(ID_ABOUT_STATUS), nullptr, nullptr);
			HWND close = CreateWindowW(L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(360), Ui(178), Ui(88), Ui(30), hwnd, reinterpret_cast<HMENU>(ID_ABOUT_CLOSE), nullptr, nullptr);
			SetWindowSubclass(title, DirectWriteStaticSubclass, 1, 1);
			SetWindowSubclass(current, DirectWriteStaticSubclass, 1, 2);
			SetWindowSubclass(repository, DirectWriteStaticSubclass, 1, 5);
			SetWindowSubclass(g_about.status, DirectWriteStaticSubclass, 1, 4);
			ApplyDarkControl(close, g_app.font);
			g_about.updateState = AboutUpdateState::Checking;
			g_about.updateUrl.clear();
			if (LaunchUpdateCacheCheck(g_about.updateProcess))
				SetTimer(hwnd, ID_ABOUT_UPDATE_TIMER, 150, nullptr);
			else
				SetAboutUpdateState(AboutUpdateState::Failed, L"检查更新失败");
			return 0;
		}
		case WM_DRAWITEM:
			if (DrawTargetToolbarButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
			break;
		case WM_ERASEBKGND:
		{
			RECT rect{};
			GetClientRect(hwnd, &rect);
			FillRect(reinterpret_cast<HDC>(wParam), &rect, g_app.windowBrush);
			return 1;
		}
		case WM_COMMAND:
			if (LOWORD(wParam) == ID_ABOUT_CLOSE)
			{
				DestroyWindow(hwnd);
				return 0;
			}
			if (LOWORD(wParam) == ID_ABOUT_REPOSITORY)
			{
				ShellExecuteW(hwnd, L"open", kGitHubRepository, nullptr, nullptr, SW_SHOWNORMAL);
				return 0;
			}
			if (LOWORD(wParam) == ID_ABOUT_STATUS && g_about.updateState == AboutUpdateState::Outdated && !g_about.updateUrl.empty())
			{
				ShellExecuteW(hwnd, L"open", g_about.updateUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				return 0;
			}
			break;
		case WM_SETCURSOR:
			if (reinterpret_cast<HWND>(wParam) == g_about.status && g_about.updateState == AboutUpdateState::Outdated)
			{
				SetCursor(LoadCursorW(nullptr, IDC_HAND));
				return TRUE;
			}
			break;
		case WM_TIMER:
			if (wParam == ID_ABOUT_UPDATE_TIMER && g_about.updateProcess &&
				WaitForSingleObject(g_about.updateProcess, 0) == WAIT_OBJECT_0)
			{
				KillTimer(hwnd, ID_ABOUT_UPDATE_TIMER);
				DWORD exitCode = 1;
				GetExitCodeProcess(g_about.updateProcess, &exitCode);
				CloseHandle(g_about.updateProcess);
				g_about.updateProcess = nullptr;
				if (exitCode == 0) CompleteAboutUpdateCheck();
				else SetAboutUpdateState(AboutUpdateState::Failed, L"检查更新失败");
				return 0;
			}
			break;
		case WM_DESTROY:
			KillTimer(hwnd, ID_ABOUT_UPDATE_TIMER);
			if (g_about.updateProcess)
			{
				CloseHandle(g_about.updateProcess);
				g_about.updateProcess = nullptr;
			}
			g_about.status = nullptr;
			g_about.window = nullptr;
			g_about.updateUrl.clear();
			return 0;
		}
		return DefWindowProcW(hwnd, message, wParam, lParam);
	}

	void ShowAboutWindow(HWND owner)
	{
		if (g_about.window && IsWindow(g_about.window))
		{
			ShowWindow(g_about.window, SW_SHOWNORMAL);
			SetForegroundWindow(g_about.window);
			return;
		}
		HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSW windowClass{};
		windowClass.hInstance = instance;
		windowClass.lpfnWndProc = AboutWindowProc;
		windowClass.lpszClassName = L"PathConfiguratorAboutWindow";
		windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		RegisterClassW(&windowClass);
		RECT desired{0, 0, Ui(470), Ui(230)};
		const DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN;
		AdjustWindowRectEx(&desired, style, FALSE, WS_EX_TOOLWINDOW);
		RECT ownerRect{};
		if (!owner || !GetWindowRect(owner, &ownerRect)) ownerRect = {0, 0, desired.right - desired.left, desired.bottom - desired.top};
		const int width = desired.right - desired.left;
		const int height = desired.bottom - desired.top;
		HWND about = CreateWindowExW(WS_EX_TOOLWINDOW, windowClass.lpszClassName, L"关于 - STM32 项目配置器", style,
			ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2,
			ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2,
			width, height, owner, nullptr, instance, nullptr);
		if (about) ShowWindow(about, SW_SHOWNORMAL);
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

	enum class BackupChoice
	{
		Cancel,
		Skip,
		Create
	};

	BackupChoice ConfirmGenerateBackups(HWND owner, const std::vector<std::wstring>& candidates)
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
		for (const std::wstring& file : files)
			message += file + L"\r\n";
		const int choice = MessageBoxW(owner, message.c_str(), L"生成文件备份",
			MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1 | MB_TOPMOST);
		if (choice == IDYES) return BackupChoice::Create;
		if (choice == IDNO) return BackupChoice::Skip;
		return BackupChoice::Cancel;
	}

	std::vector<std::wstring> ToolchainConfigurationFiles()
	{
		return {
			g_app.workspace.settingsPath,
			g_app.workspace.presetsPath,
			JoinPath(g_app.workspace.toolchainRoot, L"cmake\\PathConfiguratorCompilerCompat.cmake"),
			JoinPath(g_app.workspace.toolchainRoot, L"CMakeLists.txt")
		};
	}

	std::vector<std::wstring> CmakeTargetConfigurationFiles()
	{
		return {
			g_app.workspace.cmakeTargetConfigPath,
			g_app.workspace.cmakeTargetModulePath,
			JoinPath(g_app.workspace.toolchainRoot, L"CMakeLists.txt")
		};
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
		g_app.selectedVirtualFolder.clear();
		PushControls();
		if (SelectedTargetCategory() == TargetSources) RefreshSourceConfigurationViews();
		else RefreshTargetList();
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
		const BackupChoice backup = ConfirmGenerateBackups(owner, {GetUserDefaultSettingsPath()});
		if (backup == BackupChoice::Cancel) {
			Status(L"已取消保存默认配置。");
			return;
		}
		const bool createBackup = backup == BackupChoice::Create;
		if (!WriteUserDefaultSettings(defaults, error, createBackup)) {
			MessageBoxW(owner, error.c_str(), L"保存默认配置", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return;
		}
		Status(L"已保存本机默认工具配置。");
	}

	bool ShouldUpdateUserDefaults(HWND owner)
	{
		ToolPaths defaults = SharedTools(g_app.tools);
		ToolPaths cached;
		std::wstring error;
		const bool hasCache = LoadUserDefaultSettings(cached, error);
		if (hasCache && SameSharedTools(defaults, cached))
			return false;
		const std::wstring prompt = L"当前工程的共享工具路径与默认配置不同。\r\n\r\n是否更新本机默认配置？\r\n\r\n" +
			DescribeSharedTools(defaults) + L"\r\n不会保存 SVD 或 target。";
		return MessageBoxW(owner, prompt.c_str(), L"更新默认配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) == IDYES;
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
		{
			QueueDarkMenuSeparatorRepaint(owner);
			return NormalizePath(path);
		}
		QueueDarkMenuSeparatorRepaint(owner);
		return {};
	}

	bool AddCmakePathValue(HWND owner, const std::wstring& selected, bool isFolder);

	bool AppendShellItemPath(IShellItem* item, std::vector<std::wstring>& paths)
	{
		if (!item) return false;
		PWSTR raw = nullptr;
		if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) || !raw) return false;
		paths.push_back(NormalizePath(raw));
		CoTaskMemFree(raw);
		return true;
	}

	bool CollectCurrentDialogPaths(IFileDialog* dialog, bool isFolder, std::vector<std::wstring>& paths)
	{
		if (isFolder)
		{
			IShellItem* item = nullptr;
			HRESULT result = dialog->GetCurrentSelection(&item);
			if (FAILED(result) || !item) result = dialog->GetFolder(&item);
			if (SUCCEEDED(result) && item)
			{
				AppendShellItemPath(item, paths);
				item->Release();
			}
			return !paths.empty();
		}

		IFileOpenDialog* openDialog = nullptr;
		if (FAILED(dialog->QueryInterface(IID_IFileOpenDialog, reinterpret_cast<void**>(&openDialog))))
			return false;
		IShellItemArray* items = nullptr;
		if (SUCCEEDED(openDialog->GetSelectedItems(&items)) && items)
		{
			DWORD count = 0;
			items->GetCount(&count);
			for (DWORD index = 0; index < count; ++index)
			{
				IShellItem* item = nullptr;
				if (SUCCEEDED(items->GetItemAt(index, &item)) && item)
				{
					AppendShellItemPath(item, paths);
					item->Release();
				}
			}
			items->Release();
		}
		openDialog->Release();
		return !paths.empty();
	}

	bool CollectFinalDialogPaths(IFileOpenDialog* dialog, bool isFolder, std::vector<std::wstring>& paths)
	{
		if (isFolder)
		{
			IShellItem* item = nullptr;
			if (SUCCEEDED(dialog->GetResult(&item)) && item)
			{
				AppendShellItemPath(item, paths);
				item->Release();
			}
			return !paths.empty();
		}

		IShellItemArray* items = nullptr;
		if (SUCCEEDED(dialog->GetResults(&items)) && items)
		{
			DWORD count = 0;
			items->GetCount(&count);
			for (DWORD index = 0; index < count; ++index)
			{
				IShellItem* item = nullptr;
				if (SUCCEEDED(items->GetItemAt(index, &item)) && item)
				{
					AppendShellItemPath(item, paths);
					item->Release();
				}
			}
			items->Release();
		}
		return !paths.empty();
	}

	bool PostCmakeDialogResult(const CMakeDialogRequest& request, std::vector<std::wstring> paths)
	{
		CMakeDialogResult* result = new CMakeDialogResult{};
		result->paths = std::move(paths);
		result->isFolder = request.isFolder;
		if (PostMessageW(request.owner, WM_APP_CMAKE_DIALOG_RESULT, 0, reinterpret_cast<LPARAM>(result)))
			return true;
		delete result;
		return false;
	}

	class CMakeDialogEvents final : public IFileDialogEvents
	{
	public:
		explicit CMakeDialogEvents(const CMakeDialogRequest& request) : request_(request) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
		{
			if (!object) return E_POINTER;
			*object = nullptr;
			if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IFileDialogEvents))
			{
				*object = static_cast<IFileDialogEvents*>(this);
				AddRef();
				return S_OK;
			}
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG references = static_cast<ULONG>(InterlockedDecrement(&references_));
			if (references == 0) delete this;
			return references;
		}

		HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog* dialog) override
		{
			if (!resultPosted_)
			{
				std::vector<std::wstring> paths;
				if (CollectCurrentDialogPaths(dialog, request_.isFolder, paths))
					resultPosted_ = PostCmakeDialogResult(request_, std::move(paths));
			}
			return S_OK;
		}

		HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* response) override
		{
			if (response) *response = FDESVR_DEFAULT;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* response) override
		{
			if (response) *response = FDEOR_DEFAULT;
			return S_OK;
		}

		bool ResultPosted() const { return resultPosted_; }

	private:
		LONG references_ = 1;
		CMakeDialogRequest request_;
		bool resultPosted_ = false;
	};

	DWORD WINAPI CMakePathDialogThread(void* parameter)
	{
		CMakeDialogRequest* request = static_cast<CMakeDialogRequest*>(parameter);
		const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		IFileOpenDialog* dialog = nullptr;
		CMakeDialogEvents* events = nullptr;
		DWORD eventCookie = 0;
		HRESULT showResult = E_FAIL;
		if (SUCCEEDED(initialized) && SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
			CLSCTX_INPROC_SERVER, IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog))))
		{
			FILEOPENDIALOGOPTIONS options{};
			dialog->GetOptions(&options);
			options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR;
			if (request->isFolder) options |= FOS_PICKFOLDERS;
			else options |= FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT;
			dialog->SetOptions(options);
			dialog->SetTitle(request->title.c_str());
			if (!request->isFolder)
			{
				const COMDLG_FILTERSPEC filters[] = {
					{L"C/C++/汇编源文件 (*.c;*.cc;*.cpp;*.s;*.asm)", L"*.c;*.cc;*.cp;*.cpp;*.cxx;*.s;*.S;*.asm"},
					{L"所有文件", L"*.*"}
				};
				dialog->SetFileTypes(static_cast<UINT>(ARRAY_SIZE(filters)), filters);
				dialog->SetFileTypeIndex(1);
			}
			IShellItem* initialItem = nullptr;
			if (SUCCEEDED(SHCreateItemFromParsingName(request->root.c_str(), nullptr,
				IID_IShellItem, reinterpret_cast<void**>(&initialItem))))
			{
				dialog->SetFolder(initialItem);
				initialItem->Release();
			}

			events = new CMakeDialogEvents(*request);
			const bool advised = SUCCEEDED(dialog->Advise(events, &eventCookie));
			showResult = dialog->Show(request->owner);
			if (SUCCEEDED(showResult) && !events->ResultPosted())
			{
				std::vector<std::wstring> paths;
				CollectFinalDialogPaths(dialog, request->isFolder, paths);
				PostCmakeDialogResult(*request, std::move(paths));
			}
			else if (FAILED(showResult) && !events->ResultPosted())
			{
				PostCmakeDialogResult(*request, {});
			}
			if (advised) dialog->Unadvise(eventCookie);
			events->Release();
		}
		else
		{
			PostCmakeDialogResult(*request, {});
		}

		if (dialog) dialog->Release();
		if (SUCCEEDED(initialized)) CoUninitialize();
		PostMessageW(request->owner, WM_APP_CMAKE_DIALOG_CLEANUP_COMPLETE, 0, 0);
		delete request;
		return 0;
	}

	void BeginCmakePathDialog(HWND owner, bool isFolder)
	{
		if (SelectedTargetCategory() == TargetSources && g_app.targetListReadOnly) return;
		if (g_app.cmakeDialogOpen)
		{
			Status(L"文件选择器正在关闭，请稍候。");
			return;
		}
		const int category = SelectedTargetCategory();
		CMakeDialogRequest* request = new CMakeDialogRequest{};
		request->owner = owner;
		request->root = g_app.workspace.root;
		request->isFolder = isFolder;
		request->title = isFolder
			? (category == TargetSources ? L"选择递归加入的源目录" : L"选择要加入 CMake 的目录")
			: L"选择要加入 CMake 目标的源文件";
		g_app.cmakeDialogOpen = true;
		++g_app.cmakeDialogWorkers;
		HANDLE thread = CreateThread(nullptr, 0, CMakePathDialogThread, request, 0, nullptr);
		if (thread)
		{
			CloseHandle(thread);
			return;
		}
		--g_app.cmakeDialogWorkers;
		g_app.cmakeDialogOpen = false;
		delete request;
		MessageBoxW(owner, L"无法创建文件选择器工作线程。", L"添加 CMake 项", MB_OK | MB_ICONERROR | MB_TOPMOST);
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
			g_app.cmakeTarget.sources.push_back({relative, isFolder, CurrentVirtualFolder()});
		} else {
			std::vector<std::wstring>* values = category == TargetIncludes ? &g_app.cmakeTarget.includeDirectories : &g_app.cmakeTarget.linkDirectories;
			for (const std::wstring& value : *values) if (SameConfigValue(value, relative)) return false;
			values->push_back(relative);
		}
		if (category == TargetSources) RefreshSourceConfigurationViews();
		else RefreshTargetList();
		Status(L"已加入 CMake 目标配置；点击“保存 CMake 目标配置”后写入工程文件。");
		return true;
	}

	bool CommitTargetListEdit(HWND owner, int row, const std::wstring& input)
	{
		if (g_app.targetListReadOnly) return false;
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
				if (row < 0 || static_cast<size_t>(row) > g_app.targetSourceRows.size()) return false;
				const size_t sourceIndex = static_cast<size_t>(row) == g_app.targetSourceRows.size()
					? values.size() : g_app.targetSourceRows[static_cast<size_t>(row)];
				for (size_t i = 0; i < values.size(); ++i)
					if (i != sourceIndex && SameVirtualFolder(values[i].virtualFolder, CurrentVirtualFolder()) &&
						values[i].isFolder == isFolder && SameConfigValue(values[i].path, relative)) return false;
				if (sourceIndex == values.size()) values.push_back({relative, isFolder, CurrentVirtualFolder()});
				else values[sourceIndex] = {relative, isFolder, CurrentVirtualFolder()};
			} else {
				std::vector<std::wstring>& values = category == TargetIncludes ? g_app.cmakeTarget.includeDirectories : g_app.cmakeTarget.linkDirectories;
				if (row < 0 || static_cast<size_t>(row) > values.size()) return false;
				for (size_t i = 0; i < values.size(); ++i)
					if (static_cast<int>(i) != row && SameConfigValue(values[i], relative)) return false;
				if (static_cast<size_t>(row) == values.size()) values.push_back(relative);
				else values[static_cast<size_t>(row)] = relative;
			}
		}
		if (category == TargetSources) RefreshSourceConfigurationViews();
		else RefreshTargetList();
		Status(L"已更新 CMake 目标配置；点击“保存 CMake 目标配置”后写入工程文件。");
		return true;
	}

	struct PhysicalSourceTarget
	{
		std::wstring path;
		bool isFolder = false;
	};

	bool IsPathWithinFolder(const std::wstring& path, const std::wstring& folder)
	{
		if (path.size() <= folder.size() || _wcsnicmp(path.c_str(), folder.c_str(), folder.size()) != 0)
			return false;
		return path[folder.size()] == L'\\' || path[folder.size()] == L'/';
	}

	std::vector<PhysicalSourceTarget> CollectPhysicalSourceTargets(const std::vector<int>& sourceIndices)
	{
		std::vector<PhysicalSourceTarget> targets;
		for (int index : sourceIndices)
		{
			if (index < 0 || static_cast<size_t>(index) >= g_app.cmakeTarget.sources.size()) continue;
			const TargetSourceEntry& source = g_app.cmakeTarget.sources[static_cast<size_t>(index)];
			const std::wstring path = NormalizePath(JoinPath(g_app.workspace.toolchainRoot, source.path));
			if ((source.isFolder && !FolderExists(path)) || (!source.isFolder && !FileExists(path))) continue;
			bool redundant = false;
			for (const PhysicalSourceTarget& existing : targets)
			{
				if (SameConfigValue(existing.path, path) || (existing.isFolder && IsPathWithinFolder(path, existing.path)))
				{
					redundant = true;
					break;
				}
			}
			if (redundant) continue;
			// 若稍后遇到父目录，移除先前收集到的内部文件/目录，避免回收站操作重复处理同一路径。
			if (source.isFolder)
			{
				targets.erase(std::remove_if(targets.begin(), targets.end(), [&path](const PhysicalSourceTarget& existing) {
					return IsPathWithinFolder(existing.path, path);
				}), targets.end());
			}
			targets.push_back({path, source.isFolder});
		}
		return targets;
	}

	std::wstring FormatPhysicalSourceTargetList(const std::vector<PhysicalSourceTarget>& targets)
	{
		std::wstring text;
		constexpr size_t shownLimit = 8;
		for (size_t i = 0; i < targets.size() && i < shownLimit; ++i)
			text += (targets[i].isFolder ? L"[目录] " : L"[文件] ") + targets[i].path + L"\r\n";
		if (targets.size() > shownLimit)
			text += L"... 以及 " + std::to_wstring(targets.size() - shownLimit) + L" 项\r\n";
		return text;
	}

	bool RecyclePhysicalTargets(HWND owner, const std::vector<PhysicalSourceTarget>& targets)
	{
		if (targets.empty()) return true;
		std::wstring paths;
		for (const PhysicalSourceTarget& target : targets)
		{
			paths += target.path;
			paths.push_back(L'\0');
		}
		paths.push_back(L'\0');
		SHFILEOPSTRUCTW operation{};
		operation.hwnd = owner;
		operation.wFunc = FO_DELETE;
		operation.pFrom = paths.c_str();
		operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
		const int result = SHFileOperationW(&operation);
		if (result != 0 || operation.fAnyOperationsAborted)
		{
			MessageBoxW(owner, L"未能完成移到回收站；未继续执行删除操作。",
				L"删除本地文件失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return false;
		}
		return true;
	}

	void EraseSourceIndices(std::vector<int> indices)
	{
		std::sort(indices.begin(), indices.end());
		indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
		for (auto index = indices.rbegin(); index != indices.rend(); ++index)
			if (*index >= 0 && static_cast<size_t>(*index) < g_app.cmakeTarget.sources.size())
				g_app.cmakeTarget.sources.erase(g_app.cmakeTarget.sources.begin() + *index);
	}

	bool DeleteSourceConfigurationItems(HWND owner, const std::vector<int>& sourceIndices, const std::wstring& title, const std::wstring& prompt)
	{
		if (sourceIndices.empty() && title.empty()) return false;
		const std::vector<PhysicalSourceTarget> physicalTargets = CollectPhysicalSourceTargets(sourceIndices);
		std::wstring message = prompt + L"\r\n\r\n是：移除工程配置，并将以下实际文件/目录移到回收站。\r\n";
		message += physicalTargets.empty() ? L"（没有可删除的实际文件或目录）\r\n" : FormatPhysicalSourceTargetList(physicalTargets);
		message += L"否：仅移除工程配置，不改动本地文件。\r\n取消：取消本次操作。";
		const int choice = MessageBoxW(owner, message.c_str(), title.empty() ? L"删除 CMake 配置" : title.c_str(),
			MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST);
		if (choice == IDCANCEL)
			return false;
		if (choice == IDYES && !RecyclePhysicalTargets(owner, physicalTargets)) return false;
		EraseSourceIndices(sourceIndices);
		RefreshSourceConfigurationViews();
		Status(choice == IDYES ? L"已删除 CMake 配置，并将实际目标移到回收站。" : L"已删除 CMake 配置，本地文件未改动。");
		return true;
	}

	void DeletePhysicalTargetsWithoutConfiguration(HWND owner, const std::vector<PhysicalSourceTarget>& targets)
	{
		if (targets.empty()) return;
		const std::wstring message = L"是否删除本地选中的 " + std::to_wstring(targets.size()) +
			L" 个文件/目录？\r\n\r\n" + FormatPhysicalSourceTargetList(targets) +
			L"是：将实际文件/目录移到回收站。\r\n否：仅移除工程配置；当前选择没有对应工程配置，因此不执行删除。\r\n取消：取消本次操作。";
		const int choice = MessageBoxW(owner, message.c_str(), L"删除本地文件/目录",
			MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST);
		if (choice == IDYES)
		{
			if (!RecyclePhysicalTargets(owner, targets)) return;
			RefreshSourceConfigurationViews();
			Status(L"已将选中的本地文件/目录移到回收站；CMake 配置未改动。");
		}
		else if (choice == IDNO)
			Status(L"当前选择没有可移除的工程配置，本地文件未改动。");
	}

	void DeletePhysicalBrowserEntries(HWND owner)
	{
		std::vector<PhysicalSourceTarget> targets;
		for (int row : SelectedTargetListRows())
		{
			if (row < 0 || static_cast<size_t>(row) >= g_app.physicalBrowserRows.size()) continue;
			const PhysicalBrowserEntry& entry = g_app.physicalBrowserRows[static_cast<size_t>(row)];
			if ((entry.isFolder && FolderExists(entry.path)) || (!entry.isFolder && FileExists(entry.path)))
				targets.push_back({entry.path, entry.isFolder});
		}
		DeletePhysicalTargetsWithoutConfiguration(owner, targets);
	}

	void DeleteVirtualFolder(HWND owner, const std::wstring& rawFolder)
	{
		const std::wstring folder = NormalizeVirtualFolderPath(rawFolder);
		if (folder.empty()) return;
		std::vector<int> sourceIndices;
		for (size_t i = 0; i < g_app.cmakeTarget.sources.size(); ++i)
			if (IsVirtualFolderOrDescendant(g_app.cmakeTarget.sources[i].virtualFolder, folder))
				sourceIndices.push_back(static_cast<int>(i));
		const std::wstring prompt = L"删除虚拟文件夹“" + folder + L"”及其下级虚拟文件夹？\r\n\r\n" +
			L"将移除 " + std::to_wstring(sourceIndices.size()) + L" 个 CMake 配置项。";
		if (!DeleteSourceConfigurationItems(owner, sourceIndices, L"删除虚拟文件夹", prompt)) return;
		g_app.cmakeTarget.virtualFolders.erase(
			std::remove_if(g_app.cmakeTarget.virtualFolders.begin(), g_app.cmakeTarget.virtualFolders.end(),
				[&folder](const std::wstring& item) { return IsVirtualFolderOrDescendant(item, folder); }),
			g_app.cmakeTarget.virtualFolders.end());
		g_app.selectedVirtualFolder = ParentVirtualFolder(folder);
		RefreshSourceConfigurationViews();
	}

	void DeleteSelectedCmakeTargetEntry(HWND owner)
	{
		if (g_app.targetListReadOnly)
		{
			if (!SelectedTargetListRows().empty())
			{
				DeletePhysicalBrowserEntries(owner);
				return;
			}
			TargetTreeItem* item = SelectedTargetTreeItem();
			if (!item || !IsPhysicalTreeFolder(item)) return;
			if (item->kind == TargetTreeItemKind::PhysicalSourceFolder && item->sourceIndex != static_cast<size_t>(-1))
			{
				DeleteSourceConfigurationItems(owner, {static_cast<int>(item->sourceIndex)}, L"删除 CMake 源目录配置",
					L"删除选中的 CMake 源目录配置？");
			}
			else if (FolderExists(item->physicalPath))
			{
				DeletePhysicalTargetsWithoutConfiguration(owner, {{item->physicalPath, true}});
			}
			return;
		}
		const int category = SelectedTargetCategory();
		const std::vector<int> indices = SelectedTargetEntryIndices();
		if (category == TargetSources)
		{
			if (!indices.empty())
			{
				const std::wstring prompt = L"删除选中的 " + std::to_wstring(indices.size()) +
					L" 个 CMake 源文件/源目录配置项？";
				DeleteSourceConfigurationItems(owner, indices, L"删除 CMake 配置项", prompt);
				return;
			}
			if (TargetTreeItem* item = SelectedTargetTreeItem(); item && item->kind == TargetTreeItemKind::VirtualFolder)
			{
				DeleteVirtualFolder(owner, item->virtualFolder);
			}
			else if (TargetTreeItem* item = SelectedTargetTreeItem(); item && item->kind == TargetTreeItemKind::Source &&
				item->sourceIndex != static_cast<size_t>(-1))
			{
				DeleteSourceConfigurationItems(owner, {static_cast<int>(item->sourceIndex)}, L"删除 CMake 源文件配置",
					L"删除选中的 CMake 源文件配置？");
			}
			return;
		}
		if (indices.empty()) return;
		std::vector<std::wstring>* values = category == TargetIncludes ? &g_app.cmakeTarget.includeDirectories :
			(category == TargetDefinitions ? &g_app.cmakeTarget.compileDefinitions : &g_app.cmakeTarget.linkDirectories);
		for (auto index = indices.rbegin(); index != indices.rend(); ++index)
			values->erase(values->begin() + *index);
		RefreshTargetList();
	}

	bool HasVirtualFolder(const std::wstring& value, const std::wstring& except = L"")
	{
		for (const std::wstring& folder : g_app.cmakeTarget.virtualFolders)
			if (!SameVirtualFolder(folder, except) && SameVirtualFolder(folder, value)) return true;
		for (const TargetSourceEntry& source : g_app.cmakeTarget.sources)
			if (!SameVirtualFolder(source.virtualFolder, except) && SameVirtualFolder(source.virtualFolder, value)) return true;
		return false;
	}

	bool WouldVirtualFolderRenameConflict(const std::wstring& oldFolder, const std::wstring& newFolder)
	{
		std::vector<std::wstring> folders;
		auto addUnique = [&folders](const std::wstring& value) {
			if (value.empty()) return;
			for (const std::wstring& existing : folders)
				if (SameVirtualFolder(existing, value)) return;
			folders.push_back(NormalizeVirtualFolderPath(value));
		};
		for (const std::wstring& folder : g_app.cmakeTarget.virtualFolders) addUnique(folder);
		for (const TargetSourceEntry& source : g_app.cmakeTarget.sources) addUnique(source.virtualFolder);
		for (std::wstring& folder : folders)
			if (IsVirtualFolderOrDescendant(folder, oldFolder))
				folder = newFolder + folder.substr(oldFolder.size());
		for (size_t i = 0; i < folders.size(); ++i)
			for (size_t j = i + 1; j < folders.size(); ++j)
				if (SameVirtualFolder(folders[i], folders[j])) return true;
		return false;
	}

	bool CommitVirtualFolderEdit(HWND owner, const std::wstring& input)
	{
		const std::wstring name = NormalizeVirtualFolderPath(input);
		if (!IsValidVirtualFolderPath(name))
		{
			MessageBoxW(owner, L"虚拟文件夹名称无效。可使用 / 创建下级分组，但不能包含 : * ? \" < > | ; $ 等字符。",
				L"虚拟文件夹", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return false;
		}
		const std::wstring oldFolder = NormalizeVirtualFolderPath(g_app.renamingVirtualFolder);
		const std::wstring parent = g_app.addingVirtualFolder ? NormalizeVirtualFolderPath(g_app.pendingVirtualFolderParent) : ParentVirtualFolder(oldFolder);
		const std::wstring newFolder = parent.empty() ? name : parent + L"/" + name;
		if (g_app.addingVirtualFolder)
		{
			if (HasVirtualFolder(newFolder))
			{
				MessageBoxW(owner, L"当前层级中已存在同名虚拟文件夹。", L"虚拟文件夹", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			g_app.cmakeTarget.virtualFolders.push_back(newFolder);
		}
		else if (!SameVirtualFolder(oldFolder, newFolder))
		{
			if (WouldVirtualFolderRenameConflict(oldFolder, newFolder))
			{
				MessageBoxW(owner, L"目标位置已存在同名虚拟文件夹。", L"虚拟文件夹", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			auto replacePrefix = [&oldFolder, &newFolder](std::wstring& value) {
				if (!IsVirtualFolderOrDescendant(value, oldFolder)) return;
				const std::wstring normalized = NormalizeVirtualFolderPath(value);
				value = newFolder + normalized.substr(oldFolder.size());
			};
			for (std::wstring& folder : g_app.cmakeTarget.virtualFolders) replacePrefix(folder);
			for (TargetSourceEntry& source : g_app.cmakeTarget.sources) replacePrefix(source.virtualFolder);
		}
		else return true;
		g_app.selectedVirtualFolder = newFolder;
		RefreshSourceConfigurationViews();
		Status(L"虚拟文件夹已更新；点击“生成”后写入 project-config.json。");
		return true;
	}

	void BeginAddVirtualFolder()
	{
		if (SelectedTargetCategory() != TargetSources || !g_app.targetTree || g_app.targetListReadOnly) return;
		TargetTreeItem* item = SelectedTargetTreeItem();
		HTREEITEM parent = TreeView_GetSelection(g_app.targetTree);
		if (item && item->kind == TargetTreeItemKind::Source)
			parent = TreeView_GetParent(g_app.targetTree, parent);
		if (!parent) parent = TreeView_GetRoot(g_app.targetTree);
		g_app.pendingVirtualFolderParent = item ? item->virtualFolder : L"";
		g_app.renamingVirtualFolder.clear();
		g_app.addingVirtualFolder = true;
		const HTREEITEM created = InsertTargetTreeItem(parent, L"新建虚拟文件夹",
			NewTargetTreeItem(TargetTreeItemKind::VirtualFolder, g_app.pendingVirtualFolderParent), true);
		TreeView_Expand(g_app.targetTree, parent, TVE_EXPAND);
		TreeView_SelectItem(g_app.targetTree, created);
		TreeView_EditLabel(g_app.targetTree, created);
	}

	void BeginAddCmakeDefinition()
	{
		if (SelectedTargetCategory() != TargetDefinitions || !g_app.targetList)
			return;
		const int emptyRow = ListView_GetItemCount(g_app.targetList) - 1;
		if (emptyRow < 0)
			return;
		ListView_SetItemState(g_app.targetList, emptyRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(g_app.targetList, emptyRow, FALSE);
		SetFocus(g_app.targetList);
		ListView_EditLabel(g_app.targetList, emptyRow);
	}

	void MoveSelectedCmakeTargetEntry(int offset)
	{
		if (!g_app.targetList || g_app.targetListReadOnly || offset == 0)
			return;
		const int category = SelectedTargetCategory();
		std::vector<int> selectedRows = SelectedTargetListRows();
		if (selectedRows.empty()) return;
		if (category == TargetSources)
		{
			const size_t count = g_app.targetSourceRows.size();
			if ((offset < 0 && selectedRows.front() == 0) ||
				(offset > 0 && static_cast<size_t>(selectedRows.back() + 1) >= count)) return;
			if (offset < 0)
			{
				for (int row : selectedRows)
					std::swap(g_app.cmakeTarget.sources[g_app.targetSourceRows[static_cast<size_t>(row)]],
						g_app.cmakeTarget.sources[g_app.targetSourceRows[static_cast<size_t>(row - 1)]]);
			}
			else
			{
				for (size_t i = selectedRows.size(); i-- > 0;)
				{
					const int row = selectedRows[i];
					std::swap(g_app.cmakeTarget.sources[g_app.targetSourceRows[static_cast<size_t>(row)]],
						g_app.cmakeTarget.sources[g_app.targetSourceRows[static_cast<size_t>(row + 1)]]);
				}
			}
			for (int& row : selectedRows) row += offset;
			RefreshSourceConfigurationViews();
			for (int row : selectedRows)
				ListView_SetItemState(g_app.targetList, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
			ListView_EnsureVisible(g_app.targetList, offset < 0 ? selectedRows.front() : selectedRows.back(), FALSE);
			UpdateTargetActionButtons();
			return;
		}

		std::vector<int> selected = SelectedTargetEntryIndices();
		const size_t count = TargetEntryCount(category);
		if ((offset < 0 && selected.front() == 0) ||
			(offset > 0 && static_cast<size_t>(selected.back() + 1) >= count))
			return;

		auto swapEntries = [category](int left, int right) {
			if (category == TargetSources)
				std::swap(g_app.cmakeTarget.sources[static_cast<size_t>(left)], g_app.cmakeTarget.sources[static_cast<size_t>(right)]);
			else if (category == TargetIncludes)
				std::swap(g_app.cmakeTarget.includeDirectories[static_cast<size_t>(left)], g_app.cmakeTarget.includeDirectories[static_cast<size_t>(right)]);
			else if (category == TargetDefinitions)
				std::swap(g_app.cmakeTarget.compileDefinitions[static_cast<size_t>(left)], g_app.cmakeTarget.compileDefinitions[static_cast<size_t>(right)]);
			else
				std::swap(g_app.cmakeTarget.linkDirectories[static_cast<size_t>(left)], g_app.cmakeTarget.linkDirectories[static_cast<size_t>(right)]);
		};

		if (offset < 0)
		{
			for (int index : selected)
				swapEntries(index, index - 1);
		}
		else
		{
			for (size_t i = selected.size(); i-- > 0;)
				swapEntries(selected[i], selected[i] + 1);
		}
		for (int& index : selected) index += offset;
		RefreshTargetList();
		for (int index : selected)
			ListView_SetItemState(g_app.targetList, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(g_app.targetList, offset < 0 ? selected.front() : selected.back(), FALSE);
		UpdateTargetActionButtons();
	}

	bool SaveCmakeTargetConfig(HWND owner)
	{
		std::wstring error;
		const BackupChoice backup = ConfirmGenerateBackups(owner, CmakeTargetConfigurationFiles());
		if (backup == BackupChoice::Cancel) {
			Status(L"已取消生成 CMake 目标配置。");
			return false;
		}
		const bool createBackup = backup == BackupChoice::Create;
		if (!WriteCMakeTargetConfig(g_app.workspace, g_app.cmakeTarget, error, createBackup)) {
			MessageBoxW(owner, error.c_str(), L"保存 CMake 目标配置失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return false;
		}
		Status(L"CMake 目标配置已保存，请在 CMake Tools 执行 Configure。");
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
		{
			QueueDarkMenuSeparatorRepaint(owner);
			return {};
		}
		QueueDarkMenuSeparatorRepaint(owner);
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
		const bool updateUserDefaults = ShouldUpdateUserDefaults(owner);
		std::vector<std::wstring> backupCandidates = ToolchainConfigurationFiles();
		if (updateUserDefaults)
			backupCandidates.push_back(GetUserDefaultSettingsPath());
		const BackupChoice backup = ConfirmGenerateBackups(owner, backupCandidates);
		if (backup == BackupChoice::Cancel)
		{
			Status(L"已取消工具链配置写入。");
			return false;
		}
		const bool createBackup = backup == BackupChoice::Create;

		std::wstring error;
		if (!WriteConfiguration(g_app.workspace, g_app.tools, g_app.projectName, g_app.chipType, g_app.svd, fromExample, error, createBackup))
		{
			MessageBoxW(owner, error.c_str(), L"写入配置失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return false;
		}
		if (updateUserDefaults && !WriteUserDefaultSettings(SharedTools(g_app.tools), error, createBackup))
		{
			MessageBoxW(owner, error.c_str(), L"更新默认配置失败", MB_OK | MB_ICONWARNING | MB_TOPMOST);
		}
		if (g_app.workspace.hasExample)
			Status(fromExample ? L"工具链配置已重建。" : L"工具链配置已修改。");
		else
			Status(L"工具链配置已生成。");
		return true;
	}

	LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_CREATE:
		{
			g_app.window = hwnd;
			// 将非客户区标题栏一并切换到深色，避免菜单深色而标题栏仍为白色。
			BOOL darkFrame = TRUE;
			DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkFrame, sizeof(darkFrame));
			// 传统 ClearType 优先对齐小字号笔画到像素网格，适合 Win32/GDI 的表格与工具界面。
			g_app.font = CreateFontW(-Ui(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
									OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
			g_app.titleFont = CreateFontW(-Ui(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
																										OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
			g_app.windowBrush = CreateSolidBrush(kThemeWindow);
			g_app.controlBrush = CreateSolidBrush(kThemeControl);
			g_app.panelBrush = CreateSolidBrush(kThemePanel);
			// 自绘区域使用 Direct2D/DirectWrite；创建失败时自动走既有的 GDI 回退路径。
			InitializeDirectWriteResources();
			g_app.tooltip = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
				L"PathConfiguratorHoverToolTip", nullptr, WS_POPUP,
				CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, nullptr, nullptr);
			g_app.menu = CreateMenu();
			g_app.configMenu = CreatePopupMenu();
			AppendDarkMenuItem(g_app.configMenu, 0, ID_CONFIG_SAVE_DEFAULT, L"保存为默认配置");
			AppendDarkMenuItem(g_app.configMenu, 0, ID_CONFIG_LOAD_DEFAULT, L"读取默认配置");
			AppendDarkMenuSeparator(g_app.configMenu);
			AppendDarkMenuItem(g_app.configMenu, 0, ID_CONFIG_CHECK_CLT, L"从注册表检查 CLT 包");
			AppendDarkMenuItem(g_app.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_app.configMenu), L"配置");
			g_app.otherMenu = CreatePopupMenu();
			AppendDarkMenuItem(g_app.otherMenu, 0, ID_OTHER_ABOUT, L"关于");
			AppendDarkMenuItem(g_app.menu, MF_POPUP, reinterpret_cast<UINT_PTR>(g_app.otherMenu), L"其它");
			ApplyDarkMenu(g_app.configMenu);
			ApplyDarkMenu(g_app.otherMenu);
			ApplyDarkMenu(g_app.menu);
			HWND title = CreateWindowW(L"STATIC", (g_app.workspace.projectLabel + L"  STM32 项目配置").c_str(), WS_CHILD | WS_VISIBLE,
				Ui(14), Ui(10), Ui(650), Ui(30), hwnd, nullptr, nullptr, nullptr);
			g_app.updateLink = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_NOTIFY | SS_RIGHT,
				Ui(675), Ui(13), Ui(220), Ui(24), hwnd, reinterpret_cast<HMENU>(ID_UPDATE_LINK), nullptr, nullptr);
			HWND subtitle = CreateWindowW(L"STATIC", (L"项目：" + g_app.projectName + L"    芯片：" + g_app.chipType).c_str(),
				WS_CHILD | WS_VISIBLE, Ui(14), Ui(40), Ui(880), Ui(20), hwnd, nullptr, nullptr, nullptr);
			SetWindowSubclass(title, DirectWriteStaticSubclass, 1, 1);
			SetWindowSubclass(subtitle, DirectWriteStaticSubclass, 1, 2);
			SetWindowSubclass(g_app.updateLink, DirectWriteStaticSubclass, 1, 3);

			g_app.pageTab = CreateWindowExW(0, WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_OWNERDRAWFIXED,
				Ui(10), Ui(66), Ui(892), Ui(34), hwnd, reinterpret_cast<HMENU>(ID_PAGE_TAB), nullptr, nullptr);
			TCITEMW toolTab{};
			toolTab.mask = TCIF_TEXT;
			toolTab.pszText = const_cast<wchar_t*>(L"工具链配置");
			TabCtrl_InsertItem(g_app.pageTab, 0, &toolTab);
			TCITEMW targetTab{};
			targetTab.mask = TCIF_TEXT;
			targetTab.pszText = const_cast<wchar_t*>(L"CMake 目标配置");
			TabCtrl_InsertItem(g_app.pageTab, 1, &targetTab);
			if (g_app.openCmakeTargetOnStartup)
				TabCtrl_SetCurSel(g_app.pageTab, 1);
			ApplyModernTheme(g_app.pageTab, g_app.font);
			SetWindowSubclass(g_app.pageTab, DarkTabSubclass, 1, 0);

			constexpr int toolLeft = 14;
			constexpr int toolEditX = 150;
			constexpr int toolEditWidth = 635;
			constexpr int toolBrowseX = 797;
			constexpr int toolBrowseWidth = 98;
			int y = Ui(111);
			for (int i = 0; i < 4; ++i)
			{
				HWND label = CreateWindowW(L"STATIC", Labels[i], WS_CHILD | WS_VISIBLE, Ui(toolLeft), y + Ui(5), Ui(126), Ui(22), hwnd, nullptr, nullptr, nullptr);
				g_app.edits[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(i).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
					Ui(toolEditX), y, Ui(toolEditWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_CMAKE + i), nullptr, nullptr);
				HWND browse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
					Ui(toolBrowseX), y, Ui(toolBrowseWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + i), nullptr, nullptr);
				ApplyModernTheme(label, g_app.font); ApplyDarkControl(g_app.edits[i], g_app.font, true); ApplyDarkControl(browse, g_app.font);
				AddToolPageControl(label); AddToolPageControl(g_app.edits[i]); AddToolPageControl(browse);
				y += Ui(39);
			}
			HWND svdLabel = CreateWindowW(L"STATIC", Labels[7], WS_CHILD | WS_VISIBLE, Ui(toolLeft), y + Ui(5), Ui(126), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[7] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(7).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				Ui(toolEditX), y, Ui(toolEditWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_SVD), nullptr, nullptr);
			HWND svdBrowse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(toolBrowseX), y, Ui(toolBrowseWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 7), nullptr, nullptr);
			ApplyModernTheme(svdLabel, g_app.font); ApplyDarkControl(g_app.edits[7], g_app.font, true); ApplyDarkControl(svdBrowse, g_app.font);
			AddToolPageControl(svdLabel); AddToolPageControl(g_app.edits[7]); AddToolPageControl(svdBrowse);
			y += Ui(39);
			HWND ocdLabel = CreateWindowW(L"STATIC", Labels[4], WS_CHILD | WS_VISIBLE, Ui(toolLeft), y + Ui(5), Ui(126), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[4] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(4).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				Ui(toolEditX), y, Ui(toolEditWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_OPENOCD), nullptr, nullptr);
			HWND ocdBrowse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(toolBrowseX), y, Ui(toolBrowseWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 4), nullptr, nullptr);
			ApplyModernTheme(ocdLabel, g_app.font); ApplyDarkControl(g_app.edits[4], g_app.font, true); ApplyDarkControl(ocdBrowse, g_app.font);
			AddToolPageControl(ocdLabel); AddToolPageControl(g_app.edits[4]); AddToolPageControl(ocdBrowse);
			y += Ui(39);
			HWND interfaceLabel = CreateWindowW(L"STATIC", Labels[5], WS_CHILD | WS_VISIBLE, Ui(toolLeft), y + Ui(5), Ui(76), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[5] = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
				Ui(101), y, Ui(238), Ui(150), hwnd, reinterpret_cast<HMENU>(ID_OPENOCD_INTERFACE), nullptr, nullptr);
			g_app.openocdConfigBrowse[0] = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(350), y, Ui(98), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 5), nullptr, nullptr);
			HWND targetLabel = CreateWindowW(L"STATIC", Labels[6], WS_CHILD | WS_VISIBLE, Ui(465), y + Ui(5), Ui(112), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[6] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", InitialValue(6).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				Ui(588), y, Ui(197), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_OPENOCD_TARGET), nullptr, nullptr);
			g_app.openocdConfigBrowse[1] = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(toolBrowseX), y, Ui(toolBrowseWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 6), nullptr, nullptr);
			ApplyModernTheme(interfaceLabel, g_app.font); ApplyDarkControl(g_app.edits[5], g_app.font); ApplyDarkControl(g_app.openocdConfigBrowse[0], g_app.font);
			ApplyModernTheme(targetLabel, g_app.font); ApplyDarkControl(g_app.edits[6], g_app.font, true); ApplyDarkControl(g_app.openocdConfigBrowse[1], g_app.font);
			AddToolPageControl(interfaceLabel); AddToolPageControl(g_app.edits[5]); AddToolPageControl(g_app.openocdConfigBrowse[0]);
			AddToolPageControl(targetLabel); AddToolPageControl(g_app.edits[6]); AddToolPageControl(g_app.openocdConfigBrowse[1]);
			HWND clear = CreateWindowW(L"BUTTON", L"清空", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(toolLeft), Ui(388), Ui(115), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_CLEAR), nullptr, nullptr);
			HWND ex = CreateWindowW(L"BUTTON", g_app.workspace.hasExample ? L"重建" : L"生成", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(g_app.workspace.hasExample ? 650 : 720), Ui(388), Ui(g_app.workspace.hasExample ? 115 : 175), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_SAVE_EXAMPLE), nullptr, nullptr);
			HWND settings = CreateWindowW(L"BUTTON", g_app.workspace.hasExample ? L"修改" : L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(g_app.workspace.hasExample ? 780 : 820), Ui(388), Ui(g_app.workspace.hasExample ? 115 : 75), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_SAVE_SETTINGS), nullptr, nullptr);
			ApplyDarkControl(clear, g_app.font); ApplyDarkControl(ex, g_app.font); ApplyDarkControl(settings, g_app.font);
			AddToolPageControl(clear); AddToolPageControl(ex); AddToolPageControl(settings);

			HWND categoryLabel = CreateWindowW(L"STATIC", L"配置类别", WS_CHILD | WS_VISIBLE, Ui(14), Ui(112), Ui(96), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.targetCategory = CreateWindowExW(0, WC_COMBOBOXW, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
				Ui(120), Ui(107), Ui(265), Ui(160), hwnd, reinterpret_cast<HMENU>(ID_TARGET_CATEGORY), nullptr, nullptr);
			const wchar_t* categories[] = {L"目标文件 / 文件夹", L"头文件目录", L"编译宏", L"链接目录"};
			for (const wchar_t* category : categories) SendMessageW(g_app.targetCategory, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(category));
			SendMessageW(g_app.targetCategory, CB_SETCURSEL, TargetSources, 0);
			g_app.targetTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_EDITLABELS,
				Ui(14), Ui(149), Ui(280), Ui(271), hwnd, reinterpret_cast<HMENU>(ID_TARGET_TREE), nullptr, nullptr);
			TreeView_SetExtendedStyle(g_app.targetTree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
			g_app.targetList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
				WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
				Ui(306), Ui(149), Ui(589), Ui(271), hwnd, nullptr, nullptr, nullptr);
			LVCOLUMNW valueColumn{};
			valueColumn.mask = LVCF_TEXT | LVCF_WIDTH;
			valueColumn.pszText = const_cast<wchar_t*>(L"当前分组中的目标项");
			valueColumn.cx = Ui(585);
			ListView_InsertColumn(g_app.targetList, 0, &valueColumn);
			ListView_SetBkColor(g_app.targetList, kThemeWindow);
			ListView_SetTextBkColor(g_app.targetList, kThemeWindow);
			ListView_SetTextColor(g_app.targetList, kThemeText);
			ListView_SetExtendedListViewStyle(g_app.targetList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			HWND targetHeader = ListView_GetHeader(g_app.targetList);
			EnableNativeDarkModeForWindow(targetHeader);
			SetWindowTheme(targetHeader, L"DarkMode_Explorer", nullptr);
			SetWindowSubclass(targetHeader, DarkHeaderSubclass, 1, 0);
			g_app.targetAddVirtualFolder = CreateWindowW(L"BUTTON", L"新建虚拟文件夹", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(358), Ui(107), Ui(36), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_TARGET_ADD_VIRTUAL_FOLDER), nullptr, nullptr);
			g_app.targetAddFile = CreateWindowW(L"BUTTON", L"添加源文件", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(400), Ui(107), Ui(36), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_TARGET_ADD_FILE), nullptr, nullptr);
			g_app.targetAddFolder = CreateWindowW(L"BUTTON", L"添加文件夹", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(442), Ui(107), Ui(36), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_TARGET_ADD_FOLDER), nullptr, nullptr);
			g_app.targetDelete = CreateWindowW(L"BUTTON", L"删除选中项", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(484), Ui(107), Ui(36), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_TARGET_DELETE), nullptr, nullptr);
			g_app.targetMoveUp = CreateWindowW(L"BUTTON", L"上移选中项", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(526), Ui(107), Ui(36), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_TARGET_MOVE_UP), nullptr, nullptr);
			g_app.targetMoveDown = CreateWindowW(L"BUTTON", L"下移选中项", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(568), Ui(107), Ui(36), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_TARGET_MOVE_DOWN), nullptr, nullptr);
			g_app.targetSave = CreateWindowW(L"BUTTON", L"生成", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(805), Ui(107), Ui(90), Ui(32), hwnd, reinterpret_cast<HMENU>(ID_TARGET_SAVE), nullptr, nullptr);
			ApplyModernTheme(categoryLabel, g_app.font); ApplyDarkControl(g_app.targetCategory, g_app.font); ApplyDarkTreeView(g_app.targetTree, g_app.font); ApplyDarkListView(g_app.targetList, g_app.font);
			ApplyDarkControl(g_app.targetAddVirtualFolder, g_app.font);
			ApplyDarkControl(g_app.targetAddFile, g_app.font); ApplyDarkControl(g_app.targetAddFolder, g_app.font);
			ApplyDarkControl(g_app.targetDelete, g_app.font); ApplyDarkControl(g_app.targetMoveUp, g_app.font);
			ApplyDarkControl(g_app.targetMoveDown, g_app.font); ApplyDarkControl(g_app.targetSave, g_app.font);
			AddToolTip(g_app.targetAddVirtualFolder, L"在当前虚拟文件夹下新建分组；不会创建磁盘目录");
			AddToolTip(g_app.targetAddFile, L"添加一个或多个 C/C++/汇编源文件");
			AddToolTip(g_app.targetAddFolder, L"递归添加源目录中的源文件");
			AddToolTip(g_app.targetDelete, L"删除选中的列表项或工程树项；可选择同步移到回收站");
			AddToolTip(g_app.targetMoveUp, L"将选中的配置项上移");
			AddToolTip(g_app.targetMoveDown, L"将选中的配置项下移");
			AddToolTip(g_app.targetSave, L"生成 CMake 目标配置并写入工程文件");
			AddTargetPageControl(categoryLabel); AddTargetPageControl(g_app.targetCategory); AddTargetPageControl(g_app.targetTree); AddTargetPageControl(g_app.targetList);
			AddTargetPageControl(g_app.targetAddVirtualFolder);
			AddTargetPageControl(g_app.targetAddFile); AddTargetPageControl(g_app.targetAddFolder); AddTargetPageControl(g_app.targetDelete);
			AddTargetPageControl(g_app.targetMoveUp); AddTargetPageControl(g_app.targetMoveDown); AddTargetPageControl(g_app.targetSave);

			PushControls();
			UpdateOpenOcdConfigControls();
			SwitchPage();

			// 顶层菜单使用客户区自绘按钮，避免原生菜单在文件对话框关闭后重绘白色底边。
			ShiftDirectChildControls(hwnd, Ui(CUSTOM_MENU_HEIGHT));
			g_app.configMenuButton = CreateWindowW(L"BUTTON", L"配置", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(5), 0, Ui(62), Ui(CUSTOM_MENU_HEIGHT), hwnd, reinterpret_cast<HMENU>(ID_CONFIG_MENU_BUTTON), nullptr, nullptr);
			g_app.otherMenuButton = CreateWindowW(L"BUTTON", L"其它", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(68), 0, Ui(62), Ui(CUSTOM_MENU_HEIGHT), hwnd, reinterpret_cast<HMENU>(ID_OTHER_MENU_BUTTON), nullptr, nullptr);
			ApplyDarkControl(g_app.configMenuButton, g_app.font);
			ApplyDarkControl(g_app.otherMenuButton, g_app.font);
			return 0;
		}
		case WM_COMMAND:
		{
			int id = LOWORD(wParam);
			if (id == ID_CONFIG_MENU_BUTTON)
			{
				ShowCustomPopupMenu(g_app.configMenuButton, g_app.configMenu);
				return 0;
			}
			if (id == ID_OTHER_MENU_BUTTON)
			{
				ShowCustomPopupMenu(g_app.otherMenuButton, g_app.otherMenu);
				return 0;
			}
			if (id == ID_UPDATE_LINK && HIWORD(wParam) == STN_CLICKED)
			{
				if (!g_app.updateUrl.empty())
					ShellExecuteW(hwnd, L"open", g_app.updateUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
				return 0;
			}
			if (id == ID_TARGET_CATEGORY && HIWORD(wParam) == CBN_SELCHANGE)
			{
				UpdateTargetCategoryControls();
				return 0;
			}
			if (id == ID_TARGET_ADD_VIRTUAL_FOLDER)
			{
				BeginAddVirtualFolder();
				return 0;
			}
			if (id == ID_TARGET_ADD_FILE)
			{
				BeginCmakePathDialog(hwnd, false);
				return 0;
			}
			if (id == ID_TARGET_ADD_FOLDER)
			{
				if (SelectedTargetCategory() == TargetDefinitions)
					BeginAddCmakeDefinition();
				else
					BeginCmakePathDialog(hwnd, true);
				return 0;
			}
			if (id == ID_TARGET_DELETE)
			{
				DeleteSelectedCmakeTargetEntry(hwnd);
				return 0;
			}
			if (id == ID_TARGET_MOVE_UP)
			{
				MoveSelectedCmakeTargetEntry(-1);
				return 0;
			}
			if (id == ID_TARGET_MOVE_DOWN)
			{
				MoveSelectedCmakeTargetEntry(1);
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
				Save(hwnd, true);
				return 0;
			}
			if (id == ID_SAVE_SETTINGS)
			{
				if (g_app.workspace.hasExample)
					Save(hwnd, false);
				else
					DestroyWindow(hwnd);
				return 0;
			}
			if (id == ID_OTHER_ABOUT)
			{
				ShowAboutWindow(hwnd);
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
		case WM_DRAWITEM:
			if (DrawPageTabItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)) ||
				DrawTargetToolbarButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)))
				return TRUE;
			if (DrawDarkMenuItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)))
				return TRUE;
			break;
		case WM_MEASUREITEM:
			if (MeasureDarkMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)))
				return TRUE;
			break;
		case WM_ERASEBKGND:
		{
			RECT client{};
			GetClientRect(hwnd, &client);
			FillRect(reinterpret_cast<HDC>(wParam), &client, g_app.windowBrush ? g_app.windowBrush : GetSysColorBrush(COLOR_WINDOW));
			return 1;
		}
		case WM_PAINT:
		{
			PAINTSTRUCT paint{};
			HDC dc = BeginPaint(hwnd, &paint);
			RECT client{};
			GetClientRect(hwnd, &client);
			client.bottom = Ui(3);
			FillRect(dc, &client, g_app.windowBrush ? g_app.windowBrush : GetSysColorBrush(COLOR_WINDOW));
			EndPaint(hwnd, &paint);
			return 0;
		}
		case WM_SIZE:
			return 0;
		case WM_NOTIFY:
		{
			const NMHDR* notification = reinterpret_cast<const NMHDR*>(lParam);
			if (notification && notification->code == NM_CUSTOMDRAW)
			{
				if (notification->hwndFrom == g_app.pageTab)
					return DrawPageTabCustom(reinterpret_cast<const NMCUSTOMDRAW*>(lParam));
				if (notification->hwndFrom == g_app.targetTree)
					return DrawTargetTreeCustom(reinterpret_cast<const NMCUSTOMDRAW*>(lParam));
				if (notification->hwndFrom == g_app.targetList)
					return DrawTargetListCustom(reinterpret_cast<const NMCUSTOMDRAW*>(lParam));
				if (g_app.targetList && notification->hwndFrom == ListView_GetHeader(g_app.targetList))
					return DrawHeaderCustom(reinterpret_cast<const NMCUSTOMDRAW*>(lParam));
			}
			if (notification && notification->idFrom == ID_PAGE_TAB && notification->code == TCN_SELCHANGE)
			{
				SwitchPage();
				return 0;
			}
			if (notification && (notification->hwndFrom == g_app.targetTree || notification->hwndFrom == g_app.targetList) &&
				(notification->code == NM_SETFOCUS || notification->code == NM_KILLFOCUS))
			{
				// 选中项失焦后改为深色，明确当前实际操作的是左树还是右侧列表。
				RedrawWindow(g_app.targetTree, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
				RedrawWindow(g_app.targetList, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == TVN_SELCHANGEDW)
			{
				const NMTREEVIEWW* change = reinterpret_cast<const NMTREEVIEWW*>(lParam);
				const TargetTreeItem* item = change ? reinterpret_cast<const TargetTreeItem*>(change->itemNew.lParam) : nullptr;
				g_app.selectedVirtualFolder = item ? NormalizeVirtualFolderPath(item->virtualFolder) : L"";
				RefreshTargetList();
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == NM_CLICK)
			{
				// 再次点击已选中的树节点只会切换焦点，不会触发 TVN_SELCHANGED；仍需清除右侧的旧选中状态。
				RefreshTargetList();
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == TVN_ITEMEXPANDINGW)
			{
				const NMTREEVIEWW* change = reinterpret_cast<const NMTREEVIEWW*>(lParam);
				TargetTreeItem* item = change ? reinterpret_cast<TargetTreeItem*>(change->itemNew.lParam) : nullptr;
				if (change && (change->action & TVE_EXPAND) && IsPhysicalTreeFolder(item))
					LoadPhysicalFolderChildren(change->itemNew.hItem, item);
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == TVN_BEGINLABELEDITW)
			{
				TargetTreeItem* item = SelectedTargetTreeItem();
				if (!item || item->kind != TargetTreeItemKind::VirtualFolder)
					return TRUE;
				if (!g_app.addingVirtualFolder)
					g_app.renamingVirtualFolder = item->virtualFolder;
				HWND edit = TreeView_GetEditControl(g_app.targetTree);
				ApplyDarkControl(edit, g_app.font, true);
				return FALSE;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == TVN_ENDLABELEDITW)
			{
				const NMTVDISPINFOW* edit = reinterpret_cast<const NMTVDISPINFOW*>(lParam);
				const bool committed = edit && edit->item.pszText && CommitVirtualFolderEdit(hwnd, edit->item.pszText);
				if (!committed) RefreshSourceConfigurationViews();
				g_app.addingVirtualFolder = false;
				g_app.pendingVirtualFolderParent.clear();
				g_app.renamingVirtualFolder.clear();
				// 刷新树已应用新名称或撤销临时项，无需让原控件再修改一次。
				return FALSE;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == NM_DBLCLK)
			{
				TargetTreeItem* item = SelectedTargetTreeItem();
				if (item && item->kind == TargetTreeItemKind::VirtualFolder && !item->virtualFolder.empty())
					TreeView_EditLabel(g_app.targetTree, TreeView_GetSelection(g_app.targetTree));
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetList && notification->code == NM_DBLCLK)
			{
				const NMLISTVIEW* click = reinterpret_cast<const NMLISTVIEW*>(lParam);
				if (!g_app.targetListReadOnly && click->iItem >= 0) ListView_EditLabel(g_app.targetList, click->iItem);
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetList && notification->code == LVN_BEGINLABELEDITW)
			{
				if (g_app.targetListReadOnly) return TRUE;
				HWND edit = ListView_GetEditControl(g_app.targetList);
				ApplyDarkControl(edit, g_app.font, true);
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetList && notification->code == LVN_ITEMCHANGED)
			{
				UpdateTargetActionButtons();
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetList && notification->code == LVN_ENDLABELEDITW)
			{
				const NMLVDISPINFOW* edit = reinterpret_cast<const NMLVDISPINFOW*>(lParam);
				if (!g_app.targetListReadOnly && edit->item.pszText) CommitTargetListEdit(hwnd, edit->item.iItem, edit->item.pszText);
				return 0;
			}
			break;
		}
		case WM_CLOSE:
			// 关闭窗口不需要生成配置，但必须立即让任务进程收到 WM_QUIT 并结束。
			DestroyWindow(hwnd);
			PostQuitMessage(0);
			return 0;
		case WM_SETCURSOR:
			if (reinterpret_cast<HWND>(wParam) == g_app.updateLink && LOWORD(lParam) == HTCLIENT)
			{
				SetCursor(LoadCursorW(nullptr, IDC_HAND));
				return TRUE;
			}
			break;
		case WM_CTLCOLORSTATIC:
		{
			HDC dc = reinterpret_cast<HDC>(wParam);
			// 使用主题背景而不是透明刷，避免 WS_CLIPCHILDREN 下隐藏页签标签留下残影。
			SetBkMode(dc, OPAQUE);
			SetBkColor(dc, kThemeWindow);
			SetTextColor(dc, reinterpret_cast<HWND>(lParam) == g_app.updateLink ? RGB(255, 106, 106) : kThemeText);
			return reinterpret_cast<LRESULT>(g_app.windowBrush ? g_app.windowBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		case WM_CTLCOLOREDIT:
		{
			HDC dc = reinterpret_cast<HDC>(wParam);
			SetTextColor(dc, kThemeText);
			SetBkColor(dc, kThemeControl);
			return reinterpret_cast<LRESULT>(g_app.controlBrush ? g_app.controlBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		case WM_CTLCOLORLISTBOX:
		{
			HDC dc = reinterpret_cast<HDC>(wParam);
			SetTextColor(dc, kThemeText);
			SetBkColor(dc, kThemeControl);
			return reinterpret_cast<LRESULT>(g_app.controlBrush ? g_app.controlBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		case WM_CTLCOLORBTN:
		{
			HDC dc = reinterpret_cast<HDC>(wParam);
			SetTextColor(dc, kThemeText);
			SetBkColor(dc, kThemeControl);
			return reinterpret_cast<LRESULT>(g_app.controlBrush ? g_app.controlBrush : GetSysColorBrush(COLOR_WINDOW));
		}
		case WM_TIMER:
			if (wParam == ID_HOVER_TOOLTIP_TIMER)
			{
				KillTimer(hwnd, ID_HOVER_TOOLTIP_TIMER);
				ShowToolTip();
				return 0;
			}
			if (wParam == ID_UPDATE_PROCESS_TIMER)
			{
				if (!g_app.updateProcess || WaitForSingleObject(g_app.updateProcess, 0) == WAIT_OBJECT_0)
				{
					KillTimer(hwnd, ID_UPDATE_PROCESS_TIMER);
					if (g_app.updateProcess)
					{
						CloseHandle(g_app.updateProcess);
						g_app.updateProcess = nullptr;
					}
					ApplyCachedUpdate();
				}
				return 0;
			}
			break;
		case WM_APP_CMAKE_DIALOG_RESULT:
		{
			CMakeDialogResult* result = reinterpret_cast<CMakeDialogResult*>(lParam);
			g_app.cmakeDialogOpen = false;
			QueueDarkMenuSeparatorRepaint(hwnd);
			if (result)
			{
				int added = 0;
				for (const std::wstring& path : result->paths)
					if (AddCmakePathValue(hwnd, path, result->isFolder))
						++added;
				if (!result->isFolder && added > 1)
					Status(L"已加入 " + std::to_wstring(added) + L" 个源文件；点击“生成”后写入工程文件。");
			}
			delete result;
			return 0;
		}
		case WM_APP_CMAKE_DIALOG_CLEANUP_COMPLETE:
			if (g_app.cmakeDialogWorkers != 0)
				--g_app.cmakeDialogWorkers;
			return 0;
		case WM_DESTROY:
			KillTimer(hwnd, ID_HOVER_TOOLTIP_TIMER);
			KillTimer(hwnd, ID_UPDATE_PROCESS_TIMER);
			if (g_about.window && IsWindow(g_about.window))
				DestroyWindow(g_about.window);
			if (g_app.updateProcess)
			{
				CloseHandle(g_app.updateProcess);
				g_app.updateProcess = nullptr;
			}
			g_app.updateLink = nullptr;
			g_app.updateUrl.clear();
			if (g_app.menu) DestroyMenu(g_app.menu);
			ReleaseDirectWriteResources();
			if (g_app.windowBrush) DeleteObject(g_app.windowBrush);
			if (g_app.controlBrush) DeleteObject(g_app.controlBrush);
			if (g_app.panelBrush) DeleteObject(g_app.panelBrush);
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
		// VS Code/CMake Tools 也可能预先创建仅含编辑器设置的 settings.json。
		// 只要当前没有有效 SVD，就从已填入的 CubeCLT 工具反查包根目录并匹配芯片。
		if (TryAutoFillSvdFromCurrentCubeClt())
			Status(L"已从 STM32CubeCLT 自动匹配当前芯片的 SVD 文件。");
		g_app.tools.svd = g_app.svd;
		// LoadWorkspace 仅成功于已生成代码的 CubeMX CMake 工程；在此基础上复用
		// ValidateTools 的路径与 OpenOCD 配置校验，并要求本机 CMake 预设仍与当前路径一致。
		g_app.openCmakeTargetOnStartup = IsCMakeUserPresetsCurrent(g_app.workspace, g_app.tools);
		HINSTANCE instance = GetModuleHandleW(nullptr);
		WNDCLASSW tooltipClass{};
		tooltipClass.hInstance = instance;
		tooltipClass.lpfnWndProc = HoverToolTipProc;
		tooltipClass.lpszClassName = L"PathConfiguratorHoverToolTip";
		tooltipClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		RegisterClassW(&tooltipClass);
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
		AdjustWindowRectEx(&desiredClient, windowStyle, FALSE, WS_EX_APPWINDOW);
		const int windowWidth = desiredClient.right - desiredClient.left;
		const int windowHeight = Ui(520);
		// VS Code 启动任务时通常位于前台；将配置器放到该显示器的工作区中心。
		MONITORINFO monitorInfo{};
		monitorInfo.cbSize = sizeof(monitorInfo);
		HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONEAREST);
		if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo))
			monitorInfo.rcWork = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
		const int windowX = monitorInfo.rcWork.left + ((monitorInfo.rcWork.right - monitorInfo.rcWork.left) - windowWidth) / 2;
		const int windowY = monitorInfo.rcWork.top + ((monitorInfo.rcWork.bottom - monitorInfo.rcWork.top) - windowHeight) / 2;
		HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, (g_app.workspace.projectLabel + L" - STM32 工具链配置").c_str(),
									windowStyle,
									windowX, windowY, windowWidth, windowHeight, nullptr, nullptr, instance, nullptr);
		if (!hwnd)
			return 1;
		ShowWindow(hwnd, SW_SHOW);
		UpdateWindow(hwnd);
		StartUpdateCheck(hwnd);
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
	// 必须在创建任何窗口前声明 DPI 感知，否则 Windows 会把整个客户区作为位图缩放。
	EnableSystemDpiAwareness();
	// 从资源管理器直接启动时不显示短暂的控制台窗口；ConPTY 环境下不会影响 VS Code 终端。
	if (HWND console = GetConsoleWindow())
		ShowWindow(console, SW_HIDE);

	INITCOMMONCONTROLSEX commonControls{};
	EnableNativeDarkControls();
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
	bool updateCacheCheck = false;
	for (int i = 1; i < argc; ++i)
	{
		std::wstring arg = argv[i];
		if (arg == L"--check-update-cache")
		{
			updateCacheCheck = true;
			continue;
		}
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
	int result = updateCacheCheck ? RunUpdateCacheCheck() : (validate ? RunValidate(root) : RunGui(root));
	if (argv)
		LocalFree(argv);
	OleUninitialize();
	return result;
}
