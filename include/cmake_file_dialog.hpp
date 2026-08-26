#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace pathconfig::dialogs {

// 对话框线程通过这两个窗口消息把选择结果和清理完成事件交回主窗口。
constexpr UINT kCmakePathDialogResultMessage = WM_APP + 2;
constexpr UINT kCmakePathDialogCleanupMessage = WM_APP + 3;

struct CmakePathDialogResult {
    std::vector<std::wstring> paths;
    bool isFolder = false;
};

// 在独立 STA 线程中打开系统文件选择器，避免其退出过程阻塞主窗口消息循环。
// 成功创建线程后返回 true；选择结果由 kCmakePathDialogResultMessage 交付给 owner。
bool StartCmakePathDialog(HWND owner, const std::wstring& root, const std::wstring& title, bool isFolder);

} // namespace pathconfig::dialogs
