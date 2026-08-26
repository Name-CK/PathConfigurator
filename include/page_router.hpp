#pragma once

#include <windows.h>

#include <vector>

namespace pathconfig::ui {

// 集中处理页面控件可见性，主窗口只负责页面专属的数据刷新。
bool IsTargetPage(HWND pageTab);
void ShowPageControls(const std::vector<HWND>& toolchainControls,
                      const std::vector<HWND>& targetControls, bool showTargetPage);

} // namespace pathconfig::ui
