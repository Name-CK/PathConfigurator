#include "page_router.hpp"

#include <commctrl.h>

namespace pathconfig::ui {

bool IsTargetPage(HWND pageTab) {
    return pageTab && TabCtrl_GetCurSel(pageTab) == 1;
}

void ShowPageControls(const std::vector<HWND>& toolchainControls,
                      const std::vector<HWND>& targetControls, bool showTargetPage) {
    for (HWND control : toolchainControls)
        if (control) ShowWindow(control, showTargetPage ? SW_HIDE : SW_SHOW);
    for (HWND control : targetControls)
        if (control) ShowWindow(control, showTargetPage ? SW_SHOW : SW_HIDE);
}

} // namespace pathconfig::ui
