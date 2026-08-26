#pragma once

#include "gui_state.hpp"

#include <commctrl.h>

#include <string>
#include <vector>

namespace pathconfig::targetconfig {

// 负责 CMake 构建目标页的树、列表和编辑命令；窗口创建、主题绘制和消息分发仍属于主窗口。
// CMake 构建目标页的四个配置类别，与下拉框的项目索引保持一致。
enum Category : int { Sources, IncludeDirectories, CompileDefinitions, LinkDirectories };

int SelectedCategory();
std::wstring ListHeaderForCategory(int category);

bool IsPhysicalTreeFolder(const gui::TargetTreeItem* item);
gui::TargetTreeItem* TreeItemFromHandle(HTREEITEM handle);
gui::TargetTreeItem* SelectedTreeItem();

void UpdateActionButtons();
void RefreshTree();
void RefreshList();
void RefreshSourceViews();
void LoadPhysicalFolderChildren(HTREEITEM parent, gui::TargetTreeItem* item);

void BeginPathDialog(HWND owner, bool isFolder);
int AddDialogPaths(HWND owner, const std::vector<std::wstring>& paths, bool isFolder);
bool CommitListEdit(HWND owner, int row, const std::wstring& input);
void BeginAddVirtualFolder();
bool CommitVirtualFolderEdit(HWND owner, const std::wstring& input);
void BeginAddDefinition();
void DeleteSelectedEntry(HWND owner);
void MoveSelectedEntry(int offset);
bool Save(HWND owner);

} // namespace pathconfig::targetconfig
