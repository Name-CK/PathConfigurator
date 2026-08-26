#include "cmake_file_dialog.hpp"

#include "core.hpp"

#include <objbase.h>
#include <shobjidl.h>

#include <utility>
#include <vector>

namespace pathconfig::dialogs {
namespace {

struct CmakePathDialogRequest {
    HWND owner = nullptr;
    std::wstring root;
    std::wstring title;
    bool isFolder = false;
};

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

bool PostDialogResult(const CmakePathDialogRequest& request, std::vector<std::wstring> paths)
{
    auto* result = new CmakePathDialogResult{};
    result->paths = std::move(paths);
    result->isFolder = request.isFolder;
    if (PostMessageW(request.owner, kCmakePathDialogResultMessage, 0, reinterpret_cast<LPARAM>(result)))
        return true;
    delete result;
    return false;
}

class CmakeDialogEvents final : public IFileDialogEvents
{
public:
    explicit CmakeDialogEvents(const CmakePathDialogRequest& request) : request_(request) {}

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
                resultPosted_ = PostDialogResult(request_, std::move(paths));
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
    CmakePathDialogRequest request_;
    bool resultPosted_ = false;
};

DWORD WINAPI CmakePathDialogThread(void* parameter)
{
    auto* request = static_cast<CmakePathDialogRequest*>(parameter);
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog* dialog = nullptr;
    CmakeDialogEvents* events = nullptr;
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

        events = new CmakeDialogEvents(*request);
        const bool advised = SUCCEEDED(dialog->Advise(events, &eventCookie));
        showResult = dialog->Show(request->owner);
        if (SUCCEEDED(showResult) && !events->ResultPosted())
        {
            std::vector<std::wstring> paths;
            CollectFinalDialogPaths(dialog, request->isFolder, paths);
            PostDialogResult(*request, std::move(paths));
        }
        else if (FAILED(showResult) && !events->ResultPosted())
        {
            PostDialogResult(*request, {});
        }
        if (advised) dialog->Unadvise(eventCookie);
        events->Release();
    }
    else
    {
        PostDialogResult(*request, {});
    }

    if (dialog) dialog->Release();
    if (SUCCEEDED(initialized)) CoUninitialize();
    PostMessageW(request->owner, kCmakePathDialogCleanupMessage, 0, 0);
    delete request;
    return 0;
}

} // namespace

bool StartCmakePathDialog(HWND owner, const std::wstring& root, const std::wstring& title, bool isFolder)
{
    auto* request = new CmakePathDialogRequest{};
    request->owner = owner;
    request->root = root;
    request->title = title;
    request->isFolder = isFolder;
    HANDLE thread = CreateThread(nullptr, 0, CmakePathDialogThread, request, 0, nullptr);
    if (thread)
    {
        CloseHandle(thread);
        return true;
    }
    delete request;
    return false;
}

} // namespace pathconfig::dialogs
