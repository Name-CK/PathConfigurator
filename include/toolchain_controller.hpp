#pragma once

#include "gui_state.hpp"

#include <string>
#include <vector>

namespace pathconfig::toolchain {

const std::wstring& InitialValue(const gui::AppState& app, int index);
void PullControls(gui::AppState& app);
void PushControls(gui::AppState& app);
void UpdateOpenOcdConfigControls(gui::AppState& app);

ToolPaths SharedTools(const ToolPaths& tools);
std::vector<std::wstring> ConfigurationFiles(const WorkspaceInfo& workspace);

void ApplyOpenOcdDefaults(gui::AppState& app);
bool TryAutoFillSvd(gui::AppState& app);
void OfferUserDefaultsOnStartup(gui::AppState& app, HWND owner, bool hasProjectSettings);
void ReadUserDefaults(gui::AppState& app, HWND owner);
void SaveUserDefaults(gui::AppState& app, HWND owner);
void OfferRegistryTools(gui::AppState& app, HWND owner, bool showMissing);

void Browse(gui::AppState& app, HWND owner, int index);
bool SaveConfiguration(gui::AppState& app, HWND owner, bool fromExample);

} // namespace pathconfig::toolchain
