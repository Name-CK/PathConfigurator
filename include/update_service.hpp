#pragma once

#include <string>

namespace pathconfig::update {

struct ReleaseInfo {
    std::wstring tag;
    std::wstring url;
};

// 网络、缓存与版本比较均不依赖 Win32 窗口，供主窗口和“关于”窗口共用。
bool ReadCachedRelease(ReleaseInfo& release);
bool IsNewerThan(const std::wstring& currentVersion, const std::wstring& candidateVersion);
std::wstring DisplayVersionTag(std::wstring tag);

// 由独立进程执行，避免网络或代理问题阻塞主 GUI 线程。
int RefreshCache();

} // namespace pathconfig::update
