#include "target_config_controller.hpp"
#include "cmake_file_dialog.hpp"
#include "core.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>

namespace pathconfig::targetconfig {
using namespace pathconfig::gui;

// 此模块集中维护构建目标页的可编辑状态和文件系统操作，避免主窗口过程承载业务规则。
namespace {

void Status(const std::wstring& text)
{
    if (gui::g_app.status) SetWindowTextW(gui::g_app.status, text.c_str());
}

} // namespace

	std::wstring ListHeaderForCategory(int category)
	{
		const std::wstring rootName = FileName(g_app.workspace.root);
		const std::wstring relativeToRoot = L"（相对于工程根目录 " +
			(rootName.empty() ? L"当前工程" : rootName) + L"）";
		switch (category)
		{
		case IncludeDirectories: return L"头文件目录" + relativeToRoot;
		case CompileDefinitions: return L"编译宏（例如 USE_HAL_DRIVER 或 KEY=VALUE）";
		case LinkDirectories: return L"链接目录" + relativeToRoot;
		default: return L"当前分组中的源文件 / 源目录";
		}
	}

	int SelectedCategory()
	{
		if (!g_app.targetCategory) return Sources;
		const LRESULT selected = SendMessageW(g_app.targetCategory, CB_GETCURSEL, 0, 0);
		return selected >= Sources && selected <= LinkDirectories ? static_cast<int>(selected) : Sources;
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

	TargetTreeItem* TreeItemFromHandle(HTREEITEM handle)
	{
		if (!g_app.targetTree || !handle) return nullptr;
		TVITEMW item{};
		item.mask = TVIF_PARAM;
		item.hItem = handle;
		return TreeView_GetItem(g_app.targetTree, &item) ? reinterpret_cast<TargetTreeItem*>(item.lParam) : nullptr;
	}

	TargetTreeItem* SelectedTreeItem()
	{
		return TreeItemFromHandle(g_app.targetTree ? TreeView_GetSelection(g_app.targetTree) : nullptr);
	}

	std::wstring CurrentVirtualFolder()
	{
		if (TargetTreeItem* item = SelectedTreeItem())
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
		case Sources: return g_app.targetListReadOnly ? g_app.physicalBrowserRows.size() : g_app.targetSourceRows.size();
		case IncludeDirectories: return g_app.cmakeTarget.includeDirectories.size();
		case CompileDefinitions: return g_app.cmakeTarget.compileDefinitions.size();
		case LinkDirectories: return g_app.cmakeTarget.linkDirectories.size();
		default: return 0;
		}
	}

	std::vector<int> SelectedTargetListRows()
	{
		std::vector<int> rows;
		if (!g_app.targetList) return rows;
		const size_t entryCount = TargetEntryCount(SelectedCategory());
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
		if (SelectedCategory() == Sources)
		{
			for (int row : rows)
				if (static_cast<size_t>(row) < g_app.targetSourceRows.size())
					indices.push_back(static_cast<int>(g_app.targetSourceRows[static_cast<size_t>(row)]));
		}
		else indices = rows;
		return indices;
	}

	void UpdateActionButtons()
	{
		if (!g_app.targetList)
			return;
		const bool sourceCategory = SelectedCategory() == Sources;
		const std::vector<int> selectedRows = SelectedTargetListRows();
		const bool hasEntry = !selectedRows.empty();
		TargetTreeItem* selectedTreeItem = sourceCategory ? SelectedTreeItem() : nullptr;
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
		const size_t count = TargetEntryCount(SelectedCategory());
		const bool canDeleteTreeItem = selectedTreeItem &&
			((selectedTreeItem->kind == TargetTreeItemKind::VirtualFolder && !selectedTreeItem->virtualFolder.empty()) ||
			 (selectedTreeItem->kind == TargetTreeItemKind::Source && selectedTreeItem->sourceIndex != static_cast<size_t>(-1)));
		if (g_app.targetDelete) EnableWindow(g_app.targetDelete, hasEntry || (!hasEntry && canDeleteTreeItem));
		if (g_app.targetMoveUp) EnableWindow(g_app.targetMoveUp, hasEntry && selectedRows.front() > 0);
		if (g_app.targetMoveDown) EnableWindow(g_app.targetMoveDown, hasEntry && static_cast<size_t>(selectedRows.back() + 1) < count);
		if (g_app.targetSave) EnableWindow(g_app.targetSave, TRUE);
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

	void RefreshTree()
	{
		if (!g_app.targetTree) return;
		const std::wstring requestedSelection = CurrentVirtualFolder();
		if (!requestedSelection.empty()) g_app.selectedVirtualFolder = requestedSelection;
		TreeView_DeleteAllItems(g_app.targetTree);
		g_app.targetTreeItems.clear();

		const HTREEITEM root = InsertTargetTreeItem(TVI_ROOT, L"工程根目录", NewTargetTreeItem(TargetTreeItemKind::Root, L""), true);
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

	void RefreshList()
	{
		if (!g_app.targetList) return;
		ListView_DeleteAllItems(g_app.targetList);
		g_app.targetSourceRows.clear();
		g_app.physicalBrowserRows.clear();
		g_app.targetListReadOnly = false;
		const int category = SelectedCategory();
		auto add = [](int row, const std::wstring& value) {
			LVITEMW item{};
			item.mask = LVIF_TEXT;
			item.iItem = row;
			item.pszText = const_cast<wchar_t*>(value.c_str());
			ListView_InsertItem(g_app.targetList, &item);
		};
		if (category == Sources)
		{
			if (TargetTreeItem* item = SelectedTreeItem(); IsPhysicalTreeFolder(item))
			{
				g_app.targetListReadOnly = true;
				SetTargetListHeader(L"当前目录内容（只读）");
				RefreshPhysicalFolderContents(item->physicalPath);
				UpdateActionButtons();
				return;
			}
			SetTargetListHeader(L"当前分组中的源文件 / 源目录");
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
			const std::wstring header = ListHeaderForCategory(category);
			SetTargetListHeader(header.c_str());
			const std::vector<std::wstring>* values = category == IncludeDirectories ? &g_app.cmakeTarget.includeDirectories :
				(category == CompileDefinitions ? &g_app.cmakeTarget.compileDefinitions : &g_app.cmakeTarget.linkDirectories);
			const bool isPath = category != CompileDefinitions;
			for (size_t i = 0; i < values->size(); ++i)
				add(static_cast<int>(i), isPath ? ProjectRelativeDisplayPath((*values)[i], true) : (*values)[i]);
		}
		// 最后一行始终作为新项输入行，双击即可使用 ListView 原位编辑器创建配置。
		add(ListView_GetItemCount(g_app.targetList), L"");
		UpdateActionButtons();
	}

	void RefreshSourceViews()
	{
		RefreshTree();
		RefreshList();
	}

	bool SamePath(const std::wstring &left, const std::wstring &right)
	{
		return _wcsicmp(NormalizePath(left).c_str(), NormalizePath(right).c_str()) == 0;
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

	std::vector<std::wstring> CmakeTargetConfigurationFiles()
	{
		return {
			g_app.workspace.cmakeTargetConfigPath,
			g_app.workspace.cmakeTargetModulePath,
			JoinPath(g_app.workspace.toolchainRoot, L"CMakeLists.txt")
		};
	}

	bool AddPathValue(HWND owner, const std::wstring& selected, bool isFolder);

	void BeginPathDialog(HWND owner, bool isFolder)
	{
		if (SelectedCategory() == Sources && g_app.targetListReadOnly) return;
		if (g_app.cmakeDialogOpen)
		{
			Status(L"文件选择器正在关闭，请稍候。");
			return;
		}
		const int category = SelectedCategory();
		const std::wstring title = isFolder
			? (category == Sources ? L"选择递归加入的源目录" : L"选择要加入构建目标的目录")
			: L"选择要加入构建目标的源文件";
		g_app.cmakeDialogOpen = true;
		++g_app.cmakeDialogWorkers;
		if (dialogs::StartCmakePathDialog(owner, g_app.workspace.root, title, isFolder))
			return;
		--g_app.cmakeDialogWorkers;
		g_app.cmakeDialogOpen = false;
		MessageBoxW(owner, L"无法创建文件选择器工作线程。", L"添加构建目标项", MB_OK | MB_ICONERROR | MB_TOPMOST);
	}

	bool AddPathValue(HWND owner, const std::wstring& selected, bool isFolder)
	{
		const int category = SelectedCategory();
		std::wstring relative;
		if (!MakeToolchainRelativePath(g_app.workspace, selected, isFolder, relative)) {
			MessageBoxW(owner, L"所选路径无法转换为可移植的 CMake 相对路径。\r\n请选择与当前工程位于同一磁盘分区的文件或目录。",
				L"无法添加构建目标项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
			return false;
		}
		if (category == Sources) {
			for (const TargetSourceEntry& entry : g_app.cmakeTarget.sources) {
				if (entry.isFolder == isFolder && SameConfigValue(entry.path, relative)) return false;
			}
			g_app.cmakeTarget.sources.push_back({relative, isFolder, CurrentVirtualFolder()});
		} else {
			std::vector<std::wstring>* values = category == IncludeDirectories ? &g_app.cmakeTarget.includeDirectories : &g_app.cmakeTarget.linkDirectories;
			for (const std::wstring& value : *values) if (SameConfigValue(value, relative)) return false;
			values->push_back(relative);
		}
		if (category == Sources) RefreshSourceViews();
		else RefreshList();
		Status(L"已加入构建目标配置；点击“生成”后写入工程文件。");
		return true;
	}

	int AddDialogPaths(HWND owner, const std::vector<std::wstring>& paths, bool isFolder)
	{
		int added = 0;
		for (const std::wstring& path : paths)
			if (AddPathValue(owner, path, isFolder)) ++added;
		if (!isFolder && added > 1)
			Status(L"已加入 " + std::to_wstring(added) + L" 个源文件；点击“生成”后写入工程文件。");
		return added;
	}

	bool CommitListEdit(HWND owner, int row, const std::wstring& input)
	{
		if (g_app.targetListReadOnly) return false;
		const std::wstring value = Trim(input);
		if (value.empty()) return false;
		const int category = SelectedCategory();
		if (category == CompileDefinitions) {
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
				MessageBoxW(owner, (L"路径不存在：\r\n" + candidate).c_str(), L"无法编辑构建目标项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			if (category != Sources && !isFolder) {
				MessageBoxW(owner, L"当前配置类别仅接受目录路径。", L"无法编辑构建目标项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			std::wstring relative;
			if (!MakeToolchainRelativePath(g_app.workspace, candidate, isFolder, relative)) {
				MessageBoxW(owner, L"所选路径无法转换为可移植的 CMake 相对路径。", L"无法编辑构建目标项", MB_OK | MB_ICONWARNING | MB_TOPMOST);
				return false;
			}
			if (category == Sources) {
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
				std::vector<std::wstring>& values = category == IncludeDirectories ? g_app.cmakeTarget.includeDirectories : g_app.cmakeTarget.linkDirectories;
				if (row < 0 || static_cast<size_t>(row) > values.size()) return false;
				for (size_t i = 0; i < values.size(); ++i)
					if (static_cast<int>(i) != row && SameConfigValue(values[i], relative)) return false;
				if (static_cast<size_t>(row) == values.size()) values.push_back(relative);
				else values[static_cast<size_t>(row)] = relative;
			}
		}
		if (category == Sources) RefreshSourceViews();
		else RefreshList();
		Status(L"已更新构建目标配置；点击“生成”后写入工程文件。");
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
		const int choice = MessageBoxW(owner, message.c_str(), title.empty() ? L"删除构建目标配置" : title.c_str(),
			MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST);
		if (choice == IDCANCEL)
			return false;
		if (choice == IDYES && !RecyclePhysicalTargets(owner, physicalTargets)) return false;
		EraseSourceIndices(sourceIndices);
		RefreshSourceViews();
		Status(choice == IDYES ? L"已删除构建目标配置，并将实际文件或目录移到回收站。" : L"已删除构建目标配置，本地文件未改动。");
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
			RefreshSourceViews();
			Status(L"已将选中的本地文件或目录移到回收站；工程构建配置未改动。");
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
			L"将移除 " + std::to_wstring(sourceIndices.size()) + L" 个构建目标配置项。";
		if (!DeleteSourceConfigurationItems(owner, sourceIndices, L"删除虚拟文件夹", prompt)) return;
		g_app.cmakeTarget.virtualFolders.erase(
			std::remove_if(g_app.cmakeTarget.virtualFolders.begin(), g_app.cmakeTarget.virtualFolders.end(),
				[&folder](const std::wstring& item) { return IsVirtualFolderOrDescendant(item, folder); }),
			g_app.cmakeTarget.virtualFolders.end());
		g_app.selectedVirtualFolder = ParentVirtualFolder(folder);
		RefreshSourceViews();
	}

	void DeleteSelectedEntry(HWND owner)
	{
		if (g_app.targetListReadOnly)
		{
			if (!SelectedTargetListRows().empty())
			{
				DeletePhysicalBrowserEntries(owner);
				return;
			}
			TargetTreeItem* item = SelectedTreeItem();
			if (!item || !IsPhysicalTreeFolder(item)) return;
			if (item->kind == TargetTreeItemKind::PhysicalSourceFolder && item->sourceIndex != static_cast<size_t>(-1))
			{
				DeleteSourceConfigurationItems(owner, {static_cast<int>(item->sourceIndex)}, L"删除源目录配置",
					L"删除选中的源目录配置？");
			}
			else if (FolderExists(item->physicalPath))
			{
				DeletePhysicalTargetsWithoutConfiguration(owner, {{item->physicalPath, true}});
			}
			return;
		}
		const int category = SelectedCategory();
		const std::vector<int> indices = SelectedTargetEntryIndices();
		if (category == Sources)
		{
			if (!indices.empty())
			{
				const std::wstring prompt = L"删除选中的 " + std::to_wstring(indices.size()) +
					L" 个源文件或源目录配置项？";
				DeleteSourceConfigurationItems(owner, indices, L"删除构建目标配置项", prompt);
				return;
			}
			if (TargetTreeItem* item = SelectedTreeItem(); item && item->kind == TargetTreeItemKind::VirtualFolder)
			{
				DeleteVirtualFolder(owner, item->virtualFolder);
			}
			else if (TargetTreeItem* item = SelectedTreeItem(); item && item->kind == TargetTreeItemKind::Source &&
				item->sourceIndex != static_cast<size_t>(-1))
			{
				DeleteSourceConfigurationItems(owner, {static_cast<int>(item->sourceIndex)}, L"删除源文件配置",
					L"删除选中的源文件配置？");
			}
			return;
		}
		if (indices.empty()) return;
		std::vector<std::wstring>* values = category == IncludeDirectories ? &g_app.cmakeTarget.includeDirectories :
			(category == CompileDefinitions ? &g_app.cmakeTarget.compileDefinitions : &g_app.cmakeTarget.linkDirectories);
		for (auto index = indices.rbegin(); index != indices.rend(); ++index)
			values->erase(values->begin() + *index);
		RefreshList();
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
		RefreshSourceViews();
		Status(L"虚拟文件夹已更新；点击“生成”后写入 project-config.json。");
		return true;
	}

	void BeginAddVirtualFolder()
	{
		if (SelectedCategory() != Sources || !g_app.targetTree || g_app.targetListReadOnly) return;
		TargetTreeItem* item = SelectedTreeItem();
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

	void BeginAddDefinition()
	{
		if (SelectedCategory() != CompileDefinitions || !g_app.targetList)
			return;
		const int emptyRow = ListView_GetItemCount(g_app.targetList) - 1;
		if (emptyRow < 0)
			return;
		ListView_SetItemState(g_app.targetList, emptyRow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(g_app.targetList, emptyRow, FALSE);
		SetFocus(g_app.targetList);
		ListView_EditLabel(g_app.targetList, emptyRow);
	}

	void MoveSelectedEntry(int offset)
	{
		if (!g_app.targetList || g_app.targetListReadOnly || offset == 0)
			return;
		const int category = SelectedCategory();
		std::vector<int> selectedRows = SelectedTargetListRows();
		if (selectedRows.empty()) return;
		if (category == Sources)
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
			RefreshSourceViews();
			for (int row : selectedRows)
				ListView_SetItemState(g_app.targetList, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
			ListView_EnsureVisible(g_app.targetList, offset < 0 ? selectedRows.front() : selectedRows.back(), FALSE);
			UpdateActionButtons();
			return;
		}

		std::vector<int> selected = SelectedTargetEntryIndices();
		const size_t count = TargetEntryCount(category);
		if ((offset < 0 && selected.front() == 0) ||
			(offset > 0 && static_cast<size_t>(selected.back() + 1) >= count))
			return;

		auto swapEntries = [category](int left, int right) {
			if (category == Sources)
				std::swap(g_app.cmakeTarget.sources[static_cast<size_t>(left)], g_app.cmakeTarget.sources[static_cast<size_t>(right)]);
			else if (category == IncludeDirectories)
				std::swap(g_app.cmakeTarget.includeDirectories[static_cast<size_t>(left)], g_app.cmakeTarget.includeDirectories[static_cast<size_t>(right)]);
			else if (category == CompileDefinitions)
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
		RefreshList();
		for (int index : selected)
			ListView_SetItemState(g_app.targetList, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(g_app.targetList, offset < 0 ? selected.front() : selected.back(), FALSE);
		UpdateActionButtons();
	}

	bool Save(HWND owner)
	{
		std::wstring error;
		const BackupChoice backup = ConfirmGenerateBackups(owner, CmakeTargetConfigurationFiles());
		if (backup == BackupChoice::Cancel) {
			Status(L"已取消生成 CMake 构建目标配置。");
			return false;
		}
		const bool createBackup = backup == BackupChoice::Create;
		if (!WriteCMakeTargetConfig(g_app.workspace, g_app.cmakeTarget, error, createBackup)) {
			MessageBoxW(owner, error.c_str(), L"生成 CMake 构建目标配置失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
			return false;
		}
		Status(L"CMake 构建目标配置已生成，请在 CMake Tools 中执行 Configure。");
		return true;
	}

} // namespace pathconfig::targetconfig
