#include "core.hpp"
#include "cmake_file_dialog.hpp"
#include "gui_state.hpp"
#include "page_router.hpp"
#include "target_config_controller.hpp"
#include "toolchain_controller.hpp"
#include "update_service.hpp"

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
#include <uxtheme.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <functional>
#include <limits>
#include <memory>
#include <utility>

using namespace pathconfig;

namespace pathconfig::gui
{
	AppState g_app;
	AboutWindowState g_about;
}

namespace
{
	using namespace pathconfig::gui;
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
		ID_ABOUT_CLOSE = 335,
		ID_OTHER_TOGGLE_THEME = 336
	};

	constexpr UINT_PTR ID_HOVER_TOOLTIP_TIMER = 1;
	constexpr UINT_PTR ID_UPDATE_PROCESS_TIMER = 2;
	constexpr UINT_PTR ID_ABOUT_UPDATE_TIMER = 3;
	constexpr int CUSTOM_MENU_HEIGHT = 20;

	constexpr wchar_t kAppVersion[] = L"1.1.0";
	constexpr wchar_t kGitHubRepository[] = L"https://github.com/Name-CK/PathConfigurator";

	enum class ThemeMode { Light, Dark };

	// 默认浅色。所有自绘控件读取同一调色板，避免主题切换时出现局部颜色不一致。
	ThemeMode g_themeMode = ThemeMode::Light;
	COLORREF kThemeWindow = RGB(248, 249, 252);
	COLORREF kThemePanel = RGB(241, 243, 247);
	COLORREF kThemeControl = RGB(255, 255, 255);
	COLORREF kThemeBorder = RGB(196, 200, 208);
	COLORREF kThemeText = RGB(31, 35, 42);
	COLORREF kThemeMutedText = RGB(99, 105, 117);
	COLORREF kThemeAccent = RGB(0, 103, 192);
	COLORREF kThemeSelection = RGB(205, 229, 250);
	COLORREF kThemeHover = RGB(228, 239, 251);
	COLORREF kThemeUpdateLink = RGB(184, 37, 37);
	const IID kIdWriteFactoryIid = {0xb859ee5a, 0xd838, 0x4b5b, {0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48}};

	bool IsDarkTheme()
	{
		return g_themeMode == ThemeMode::Dark;
	}

	void SetThemePalette(ThemeMode mode)
	{
		g_themeMode = mode;
		if (mode == ThemeMode::Dark)
		{
			kThemeWindow = RGB(30, 30, 30);
			kThemePanel = RGB(37, 37, 38);
			kThemeControl = RGB(45, 45, 48);
			kThemeBorder = RGB(70, 70, 74);
			kThemeText = RGB(232, 232, 232);
			kThemeMutedText = RGB(150, 150, 150);
			kThemeAccent = RGB(0, 122, 204);
			kThemeSelection = RGB(38, 79, 120);
			kThemeHover = RGB(55, 55, 58);
			kThemeUpdateLink = RGB(255, 106, 106);
		}
		else
		{
			kThemeWindow = RGB(248, 249, 252);
			kThemePanel = RGB(241, 243, 247);
			kThemeControl = RGB(255, 255, 255);
			kThemeBorder = RGB(196, 200, 208);
			kThemeText = RGB(31, 35, 42);
			kThemeMutedText = RGB(99, 105, 117);
			kThemeAccent = RGB(0, 103, 192);
			kThemeSelection = RGB(205, 229, 250);
			kThemeHover = RGB(228, 239, 251);
			kThemeUpdateLink = RGB(184, 37, 37);
		}
	}

	std::wstring ThemeSettingsPath()
	{
		const std::wstring defaults = GetUserDefaultSettingsPath();
		const std::wstring directory = GetParentPath(defaults);
		return directory.empty() ? std::wstring{} : JoinPath(directory, L"ui-settings.json");
	}

	void LoadThemePreference()
	{
		SetThemePalette(ThemeMode::Light);
		const std::wstring path = ThemeSettingsPath();
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return;
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4096) { CloseHandle(file); return; }
		std::string document(static_cast<size_t>(size.QuadPart), '\0');
		DWORD read = 0;
		const bool ok = ReadFile(file, document.data(), static_cast<DWORD>(document.size()), &read, nullptr) && read == document.size();
		CloseHandle(file);
		if (ok && document.find("\"theme\"") != std::string::npos && document.find("\"dark\"") != std::string::npos)
			SetThemePalette(ThemeMode::Dark);
	}

	bool SaveThemePreference()
	{
		const std::wstring path = ThemeSettingsPath();
		if (path.empty()) return false;
		const std::wstring directory = GetParentPath(path);
		if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		const std::string document = std::string("{\n  \"theme\": \"") + (IsDarkTheme() ? "dark" : "light") + "\"\n}\n";
		const std::wstring temporary = path + L".tmp";
		HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		DWORD written = 0;
		const bool ok = WriteFile(file, document.data(), static_cast<DWORD>(document.size()), &written, nullptr) && written == document.size();
		FlushFileBuffers(file);
		CloseHandle(file);
		if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		{
			DeleteFileW(temporary.c_str());
			return false;
		}
		return true;
	}

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

	const wchar_t *Labels[] = {L"CMake", L"Ninja", L"starm-clang", L"GNU Arm GDB", L"OpenOCD", L"调试接口", L"目标芯片配置", L"SVD"};

	void AddToolPageControl(HWND control)
	{
		if (control) g_app.toolPageControls.push_back(control);
	}

	void AddTargetPageControl(HWND control)
	{
		if (control) g_app.targetPageControls.push_back(control);
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
			if (isUpdateLink) color = kThemeUpdateLink;
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
		const int category = targetconfig::SelectedCategory();
		const bool sources = category == targetconfig::Sources;
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
			const std::wstring header = targetconfig::ListHeaderForCategory(category);
			column.pszText = const_cast<wchar_t*>(header.c_str());
			ListView_SetColumn(g_app.targetList, 0, &column);
		}
		if (g_app.targetAddFolder)
		{
			const bool definitions = category == targetconfig::CompileDefinitions;
			SetWindowTextW(g_app.targetAddFolder, definitions ? L"添加编译宏" : (sources ? L"添加源目录" : L"添加目录"));
			UpdateToolTip(g_app.targetAddFolder, definitions ? L"添加编译宏" :
				(sources ? L"递归添加源目录" : L"添加目录"));
		}
		// 不支持文件的类别不显示“添加文件”和“新建虚拟文件夹”。
		LayoutTargetToolbar(sources);
		if (sources) targetconfig::RefreshSourceViews();
		else targetconfig::RefreshList();
		RedrawWindow(g_app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}

	void SwitchPage()
	{
		const bool targetPage = ui::IsTargetPage(g_app.pageTab);
		ui::ShowPageControls(g_app.toolPageControls, g_app.targetPageControls, targetPage);
		if (targetPage) UpdateTargetCategoryControls();
		// 透明标签隐藏时，父窗口也必须重绘，否则可能残留“配置类别”等旧文本。
		RedrawWindow(g_app.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
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
	void SetNativeAppTheme(bool dark)
	{
		using SetPreferredAppModeFn = int (WINAPI*)(int);
		using FlushMenuThemesFn = void (WINAPI*)();
		HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
		if (!theme) return;
		auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
			GetProcAddress(theme, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(135))));
		// PreferredAppMode: ForceDark = 2，ForceLight = 3。
		if (setPreferredAppMode) setPreferredAppMode(dark ? 2 : 3);
		auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
			GetProcAddress(theme, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(136))));
		if (flushMenuThemes) flushMenuThemes();
	}

	void SetNativeDarkModeForWindow(HWND control, bool dark)
	{
		using AllowDarkModeForWindowFn = BOOL (WINAPI*)(HWND, BOOL);
		HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
		if (!theme || !control) return;
		auto allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
			GetProcAddress(theme, reinterpret_cast<LPCSTR>(static_cast<ULONG_PTR>(133))));
		if (allowDarkModeForWindow) allowDarkModeForWindow(control, dark ? TRUE : FALSE);
	}

	void ApplyDarkListView(HWND control, HFONT font)
	{
		if (!control) return;
		LONG_PTR style = GetWindowLongPtrW(control, GWL_EXSTYLE);
		style &= ~static_cast<LONG_PTR>(WS_EX_CLIENTEDGE);
		SetWindowLongPtrW(control, GWL_EXSTYLE, style);
		SetWindowPos(control, nullptr, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
		SetNativeDarkModeForWindow(control, IsDarkTheme());
		SetWindowTheme(control, IsDarkTheme() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
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
		SetNativeDarkModeForWindow(control, IsDarkTheme());
		SetWindowTheme(control, IsDarkTheme() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
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

	const wchar_t* ThemeToggleMenuText()
	{
		return IsDarkTheme() ? L"切换到浅色主题" : L"切换到深色主题";
	}

	void RefreshThemeResources(HWND owner)
	{
		SetNativeAppTheme(IsDarkTheme());
		const BOOL darkFrame = IsDarkTheme() ? TRUE : FALSE;
		if (g_app.window)
			DwmSetWindowAttribute(g_app.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkFrame, sizeof(darkFrame));
		if (g_about.window)
			DwmSetWindowAttribute(g_about.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkFrame, sizeof(darkFrame));

		if (g_app.windowBrush) DeleteObject(g_app.windowBrush);
		if (g_app.controlBrush) DeleteObject(g_app.controlBrush);
		if (g_app.panelBrush) DeleteObject(g_app.panelBrush);
		g_app.windowBrush = CreateSolidBrush(kThemeWindow);
		g_app.controlBrush = CreateSolidBrush(kThemeControl);
		g_app.panelBrush = CreateSolidBrush(kThemePanel);

		if (g_app.targetTree)
		{
			SetNativeDarkModeForWindow(g_app.targetTree, IsDarkTheme());
			SetWindowTheme(g_app.targetTree, IsDarkTheme() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
			TreeView_SetBkColor(g_app.targetTree, kThemeWindow);
			TreeView_SetTextColor(g_app.targetTree, kThemeText);
			TreeView_SetLineColor(g_app.targetTree, kThemeBorder);
		}
		if (g_app.targetList)
		{
			ListView_SetBkColor(g_app.targetList, kThemeWindow);
			ListView_SetTextBkColor(g_app.targetList, kThemeWindow);
			ListView_SetTextColor(g_app.targetList, kThemeText);
			HWND header = ListView_GetHeader(g_app.targetList);
			SetNativeDarkModeForWindow(header, IsDarkTheme());
			SetWindowTheme(header, IsDarkTheme() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
		}
		ApplyDarkMenu(g_app.configMenu);
		ApplyDarkMenu(g_app.otherMenu);
		ApplyDarkMenu(g_app.menu);
		if (g_app.otherMenu)
			ModifyMenuW(g_app.otherMenu, ID_OTHER_TOGGLE_THEME, MF_BYCOMMAND | MF_OWNERDRAW,
				ID_OTHER_TOGGLE_THEME, ThemeToggleMenuText());
		if (owner)
			RedrawWindow(owner, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
		if (g_about.window)
			RedrawWindow(g_about.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
	}

	void ToggleTheme(HWND owner)
	{
		SetThemePalette(IsDarkTheme() ? ThemeMode::Light : ThemeMode::Dark);
		RefreshThemeResources(owner);
		if (!SaveThemePreference())
			MessageBoxW(owner, L"主题已切换，但无法保存到用户目录。", L"保存主题设置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
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
			if (targetconfig::SelectedCategory() == targetconfig::CompileDefinitions) DrawToolbarMacro(item->hDC, icon, centerX, centerY);
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
		const TargetTreeItem* item = targetconfig::TreeItemFromHandle(reinterpret_cast<HTREEITEM>(custom->dwItemSpec));
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

	void Status(const std::wstring &text)
	{
		if (g_app.status)
			SetWindowTextW(g_app.status, text.c_str());
	}

	void ApplyCachedUpdate()
	{
		update::ReleaseInfo latest;
		if (!g_app.updateLink || !update::ReadCachedRelease(latest) ||
			!update::IsNewerThan(kAppVersion, latest.tag))
		{
			if (g_app.updateLink) ShowWindow(g_app.updateLink, SW_HIDE);
			return;
		}
		SetWindowTextW(g_app.updateLink, (L"有最新版 " + update::DisplayVersionTag(latest.tag)).c_str());
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
		update::ReleaseInfo latest;
		if (!update::ReadCachedRelease(latest))
		{
			SetAboutUpdateState(AboutUpdateState::Failed, L"检查更新失败");
			return;
		}
		const std::wstring latestTag = update::DisplayVersionTag(latest.tag);
		if (!update::IsNewerThan(kAppVersion, latest.tag))
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
			BOOL darkFrame = IsDarkTheme() ? TRUE : FALSE;
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


	void ClearCurrentConfiguration(HWND owner)
	{
		if (MessageBoxW(owner, L"清空当前界面中的全部路径、调试配置和 CMake 构建目标配置？\r\n\r\n此操作不会修改或删除任何 settings.json、默认配置、CMakeUserPresets.json 或 project-config.json 文件。",
			L"清空配置", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST) != IDYES)
			return;
		g_app.tools = {};
		g_app.svd.clear();
		g_app.cmakeTarget = {};
		g_app.selectedVirtualFolder.clear();
		toolchain::PushControls(g_app);
		if (targetconfig::SelectedCategory() == targetconfig::Sources) targetconfig::RefreshSourceViews();
		else targetconfig::RefreshList();
		Status(L"已清空当前界面配置；尚未修改任何文件。");
	}


	LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		switch (msg)
		{
		case WM_CREATE:
		{
			g_app.window = hwnd;
			// 非客户区标题栏跟随当前主题，避免客户区与系统标题栏颜色割裂。
			BOOL darkFrame = IsDarkTheme() ? TRUE : FALSE;
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
			AppendDarkMenuItem(g_app.otherMenu, 0, ID_OTHER_TOGGLE_THEME, ThemeToggleMenuText());
			AppendDarkMenuSeparator(g_app.otherMenu);
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
			targetTab.pszText = const_cast<wchar_t*>(L"CMake 构建目标配置");
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
				g_app.edits[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", toolchain::InitialValue(g_app, i).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
					Ui(toolEditX), y, Ui(toolEditWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_CMAKE + i), nullptr, nullptr);
				HWND browse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
					Ui(toolBrowseX), y, Ui(toolBrowseWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + i), nullptr, nullptr);
				ApplyModernTheme(label, g_app.font); ApplyDarkControl(g_app.edits[i], g_app.font, true); ApplyDarkControl(browse, g_app.font);
				AddToolPageControl(label); AddToolPageControl(g_app.edits[i]); AddToolPageControl(browse);
				y += Ui(39);
			}
			HWND svdLabel = CreateWindowW(L"STATIC", Labels[7], WS_CHILD | WS_VISIBLE, Ui(toolLeft), y + Ui(5), Ui(126), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[7] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", toolchain::InitialValue(g_app, 7).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
				Ui(toolEditX), y, Ui(toolEditWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_SVD), nullptr, nullptr);
			HWND svdBrowse = CreateWindowW(L"BUTTON", L"选择...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
				Ui(toolBrowseX), y, Ui(toolBrowseWidth), Ui(28), hwnd, reinterpret_cast<HMENU>(ID_BROWSE_BASE + 7), nullptr, nullptr);
			ApplyModernTheme(svdLabel, g_app.font); ApplyDarkControl(g_app.edits[7], g_app.font, true); ApplyDarkControl(svdBrowse, g_app.font);
			AddToolPageControl(svdLabel); AddToolPageControl(g_app.edits[7]); AddToolPageControl(svdBrowse);
			y += Ui(39);
			HWND ocdLabel = CreateWindowW(L"STATIC", Labels[4], WS_CHILD | WS_VISIBLE, Ui(toolLeft), y + Ui(5), Ui(126), Ui(22), hwnd, nullptr, nullptr, nullptr);
			g_app.edits[4] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", toolchain::InitialValue(g_app, 4).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
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
			g_app.edits[6] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", toolchain::InitialValue(g_app, 6).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
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
			const wchar_t* categories[] = {L"源文件 / 源目录", L"头文件目录", L"编译宏", L"链接目录"};
			for (const wchar_t* category : categories) SendMessageW(g_app.targetCategory, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(category));
			SendMessageW(g_app.targetCategory, CB_SETCURSEL, targetconfig::Sources, 0);
			g_app.targetTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, nullptr,
				WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_EDITLABELS,
				Ui(14), Ui(149), Ui(280), Ui(271), hwnd, reinterpret_cast<HMENU>(ID_TARGET_TREE), nullptr, nullptr);
			TreeView_SetExtendedStyle(g_app.targetTree, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
			g_app.targetList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
				WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_EDITLABELS,
				Ui(306), Ui(149), Ui(589), Ui(271), hwnd, nullptr, nullptr, nullptr);
			LVCOLUMNW valueColumn{};
			valueColumn.mask = LVCF_TEXT | LVCF_WIDTH;
			valueColumn.pszText = const_cast<wchar_t*>(L"当前分组中的源文件 / 源目录");
			valueColumn.cx = Ui(585);
			ListView_InsertColumn(g_app.targetList, 0, &valueColumn);
			ListView_SetBkColor(g_app.targetList, kThemeWindow);
			ListView_SetTextBkColor(g_app.targetList, kThemeWindow);
			ListView_SetTextColor(g_app.targetList, kThemeText);
			ListView_SetExtendedListViewStyle(g_app.targetList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
			HWND targetHeader = ListView_GetHeader(g_app.targetList);
			SetNativeDarkModeForWindow(targetHeader, IsDarkTheme());
			SetWindowTheme(targetHeader, IsDarkTheme() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
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
			AddToolTip(g_app.targetSave, L"生成 CMake 构建目标配置并写入工程文件");
			AddTargetPageControl(categoryLabel); AddTargetPageControl(g_app.targetCategory); AddTargetPageControl(g_app.targetTree); AddTargetPageControl(g_app.targetList);
			AddTargetPageControl(g_app.targetAddVirtualFolder);
			AddTargetPageControl(g_app.targetAddFile); AddTargetPageControl(g_app.targetAddFolder); AddTargetPageControl(g_app.targetDelete);
			AddTargetPageControl(g_app.targetMoveUp); AddTargetPageControl(g_app.targetMoveDown); AddTargetPageControl(g_app.targetSave);

			toolchain::PushControls(g_app);
			toolchain::UpdateOpenOcdConfigControls(g_app);
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
				targetconfig::BeginAddVirtualFolder();
				return 0;
			}
			if (id == ID_TARGET_ADD_FILE)
			{
				targetconfig::BeginPathDialog(hwnd, false);
				return 0;
			}
			if (id == ID_TARGET_ADD_FOLDER)
			{
				if (targetconfig::SelectedCategory() == targetconfig::CompileDefinitions)
					targetconfig::BeginAddDefinition();
				else
					targetconfig::BeginPathDialog(hwnd, true);
				return 0;
			}
			if (id == ID_TARGET_DELETE)
			{
				targetconfig::DeleteSelectedEntry(hwnd);
				return 0;
			}
			if (id == ID_TARGET_MOVE_UP)
			{
				targetconfig::MoveSelectedEntry(-1);
				return 0;
			}
			if (id == ID_TARGET_MOVE_DOWN)
			{
				targetconfig::MoveSelectedEntry(1);
				return 0;
			}
			if (id == ID_TARGET_SAVE)
			{
				targetconfig::Save(hwnd);
				return 0;
			}
			if (id == ID_OPENOCD && HIWORD(wParam) == EN_CHANGE)
			{
				toolchain::UpdateOpenOcdConfigControls(g_app);
				return 0;
			}
			if (id >= ID_BROWSE_BASE && id < ID_BROWSE_BASE + 8)
			{
				toolchain::Browse(g_app, hwnd, id - ID_BROWSE_BASE);
				return 0;
			}
			if (id == ID_SAVE_EXAMPLE)
			{
				toolchain::SaveConfiguration(g_app, hwnd, true);
				return 0;
			}
			if (id == ID_SAVE_SETTINGS)
			{
				if (g_app.workspace.hasExample)
					toolchain::SaveConfiguration(g_app, hwnd, false);
				else
					DestroyWindow(hwnd);
				return 0;
			}
			if (id == ID_OTHER_ABOUT)
			{
				ShowAboutWindow(hwnd);
				return 0;
			}
			if (id == ID_OTHER_TOGGLE_THEME)
			{
				ToggleTheme(hwnd);
				return 0;
			}
			if (id == ID_CLEAR)
			{
				ClearCurrentConfiguration(hwnd);
				return 0;
			}
			if (id == ID_CONFIG_SAVE_DEFAULT)
			{
				toolchain::SaveUserDefaults(g_app, hwnd);
				return 0;
			}
			if (id == ID_CONFIG_LOAD_DEFAULT)
			{
				toolchain::ReadUserDefaults(g_app, hwnd);
				return 0;
			}
			if (id == ID_CONFIG_CHECK_CLT)
			{
				toolchain::PullControls(g_app);
				SanitizeToolPaths(g_app.tools);
				g_app.svd = g_app.tools.svd;
				toolchain::PushControls(g_app);
				toolchain::OfferRegistryTools(g_app, hwnd, true);
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
				g_app.selectedVirtualFolder = item ? item->virtualFolder : L"";
				targetconfig::RefreshList();
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == NM_CLICK)
			{
				// 再次点击已选中的树节点只会切换焦点，不会触发 TVN_SELCHANGED；仍需清除右侧的旧选中状态。
				targetconfig::RefreshList();
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == TVN_ITEMEXPANDINGW)
			{
				const NMTREEVIEWW* change = reinterpret_cast<const NMTREEVIEWW*>(lParam);
				TargetTreeItem* item = change ? reinterpret_cast<TargetTreeItem*>(change->itemNew.lParam) : nullptr;
				if (change && (change->action & TVE_EXPAND) && targetconfig::IsPhysicalTreeFolder(item))
					targetconfig::LoadPhysicalFolderChildren(change->itemNew.hItem, item);
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == TVN_BEGINLABELEDITW)
			{
				TargetTreeItem* item = targetconfig::SelectedTreeItem();
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
				const bool committed = edit && edit->item.pszText && targetconfig::CommitVirtualFolderEdit(hwnd, edit->item.pszText);
				if (!committed) targetconfig::RefreshSourceViews();
				g_app.addingVirtualFolder = false;
				g_app.pendingVirtualFolderParent.clear();
				g_app.renamingVirtualFolder.clear();
				// 刷新树已应用新名称或撤销临时项，无需让原控件再修改一次。
				return FALSE;
			}
			if (notification && notification->hwndFrom == g_app.targetTree && notification->code == NM_DBLCLK)
			{
				TargetTreeItem* item = targetconfig::SelectedTreeItem();
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
				targetconfig::UpdateActionButtons();
				return 0;
			}
			if (notification && notification->hwndFrom == g_app.targetList && notification->code == LVN_ENDLABELEDITW)
			{
				const NMLVDISPINFOW* edit = reinterpret_cast<const NMLVDISPINFOW*>(lParam);
				if (!g_app.targetListReadOnly && edit->item.pszText) targetconfig::CommitListEdit(hwnd, edit->item.iItem, edit->item.pszText);
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
			SetTextColor(dc, reinterpret_cast<HWND>(lParam) == g_app.updateLink ? kThemeUpdateLink : kThemeText);
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
		case dialogs::kCmakePathDialogResultMessage:
		{
			dialogs::CmakePathDialogResult* result = reinterpret_cast<dialogs::CmakePathDialogResult*>(lParam);
			g_app.cmakeDialogOpen = false;
			QueueDarkMenuSeparatorRepaint(hwnd);
			if (result)
			{
				targetconfig::AddDialogPaths(hwnd, result->paths, result->isFolder);
			}
			delete result;
			return 0;
		}
		case dialogs::kCmakePathDialogCleanupMessage:
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
			MessageBoxW(nullptr, error.c_str(), L"CMake 构建目标配置", MB_OK | MB_ICONWARNING | MB_TOPMOST);
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
		toolchain::ApplyOpenOcdDefaults(g_app);
		// 仅当工程配置缺失或有无效工具路径时，才按默认配置、注册表的顺序补齐。
		toolchain::OfferUserDefaultsOnStartup(g_app, nullptr, hasLocalSettings);
		// VS Code/CMake Tools 也可能预先创建仅含编辑器设置的 settings.json。
		// 只要当前没有有效 SVD，就从已填入的 CubeCLT 工具反查包根目录并匹配芯片。
		if (toolchain::TryAutoFillSvd(g_app))
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
		toolchain::PushControls(g_app);
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
	// 主题独立于工程配置，始终从当前用户目录读取；缺失时保持浅色默认值。
	LoadThemePreference();
	// 从资源管理器直接启动时不显示短暂的控制台窗口；ConPTY 环境下不会影响 VS Code 终端。
	if (HWND console = GetConsoleWindow())
		ShowWindow(console, SW_HIDE);

	INITCOMMONCONTROLSEX commonControls{};
	SetNativeAppTheme(IsDarkTheme());
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
	int result = updateCacheCheck ? update::RefreshCache() : (validate ? RunValidate(root) : RunGui(root));
	if (argv)
		LocalFree(argv);
	OleUninitialize();
	return result;
}
