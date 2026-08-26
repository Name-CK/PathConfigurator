#pragma once

#include "core.hpp"

#include <d2d1.h>
#include <dwrite.h>

#include <memory>
#include <utility>
#include <vector>

namespace pathconfig::gui {

enum class TargetTreeItemKind { Root, VirtualFolder, Source, PhysicalSourceFolder, PhysicalFolder };

struct TargetTreeItem {
    TargetTreeItemKind kind = TargetTreeItemKind::Root;
    std::wstring virtualFolder;
    size_t sourceIndex = static_cast<size_t>(-1);
    std::wstring physicalPath;
    bool physicalChildrenLoaded = false;
};

struct PhysicalBrowserEntry {
    std::wstring path;
    bool isFolder = false;
};

// 仅保存窗口实例和当前编辑会话数据；路径解析、文件写入等业务逻辑保留在 core 模块。
struct AppState {
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
    bool openCmakeTargetOnStartup = false;
};

enum class AboutUpdateState { Checking, Latest, Outdated, Failed };

struct AboutWindowState {
    HWND window = nullptr;
    HWND status = nullptr;
    HANDLE updateProcess = nullptr;
    std::wstring updateUrl;
    AboutUpdateState updateState = AboutUpdateState::Checking;
};

extern AppState g_app;
extern AboutWindowState g_about;

} // namespace pathconfig::gui
