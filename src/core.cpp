#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <utility>

#include <shlwapi.h>

namespace pathconfig {
namespace {

std::string Utf8FromWide(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring WideFromUtf8(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    if (n > 0) {
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n) <= 0)
            MultiByteToWideChar(CP_ACP, 0, value.data(), static_cast<int>(value.size()), out.data(), n);
    }
    return out;
}

bool ReadBytes(const std::wstring& path, std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 * 1024 * 1024) { CloseHandle(h); return false; }
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = data.empty() || ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    data.resize(read);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);
    return true;
}

bool WriteBytesAtomic(const std::wstring& path, const std::string& data) {
    std::wstring temp = path + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != data.size()) { DeleteFileW(temp.c_str()); return false; }
    std::wstring backup = path + L".bak";
    if (FileExists(path)) CopyFileW(path.c_str(), backup.c_str(), FALSE);
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str()); return false;
    }
    return true;
}

size_t SkipSpace(const std::string& s, size_t p) {
    while (p < s.size()) {
        if (std::isspace(static_cast<unsigned char>(s[p]))) { ++p; continue; }
        if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '/') { p += 2; while (p < s.size() && s[p] != '\n') ++p; continue; }
        if (p + 1 < s.size() && s[p] == '/' && s[p + 1] == '*') { p += 2; while (p + 1 < s.size() && !(s[p] == '*' && s[p + 1] == '/')) ++p; p = std::min(p + 2, s.size()); continue; }
        break;
    }
    return p;
}

std::string JsonEscape(const std::wstring& value) {
    std::string u = Utf8FromWide(value), out;
    out.reserve(u.size() + 8);
    for (unsigned char c : u) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else out.push_back(static_cast<char>(c));
    }
    return out;
}

bool FindJsonString(const std::string& document, const std::string& key, std::wstring& value) {
    const std::string needle = "\"" + key + "\"";
    size_t p = 0;
    while ((p = document.find(needle, p)) != std::string::npos) {
        size_t colon = SkipSpace(document, p + needle.size());
        if (colon >= document.size() || document[colon] != ':') { p += needle.size(); continue; }
        size_t q = SkipSpace(document, colon + 1);
        if (q >= document.size() || document[q] != '"') { p += needle.size(); continue; }
        ++q; std::string raw;
        bool escaped = false;
        for (; q < document.size(); ++q) {
            char c = document[q];
            if (!escaped && c == '"') { value = WideFromUtf8(raw); return true; }
            if (!escaped && c == '\\') { escaped = true; continue; }
            if (escaped) {
                if (c == 'n') raw.push_back('\n');
                else if (c == 'r') raw.push_back('\r');
                else if (c == 't') raw.push_back('\t');
                else raw.push_back(c);
                escaped = false;
            } else raw.push_back(c);
        }
        return false;
    }
    return false;
}

void ReplaceJsonString(std::string& document, const std::string& key, const std::wstring& value) {
    const std::string needle = "\"" + key + "\"";
    size_t p = 0;
    while ((p = document.find(needle, p)) != std::string::npos) {
        size_t colon = SkipSpace(document, p + needle.size());
        size_t q = SkipSpace(document, colon + 1);
        if (colon < document.size() && document[colon] == ':' && q < document.size() && document[q] == '"') {
            size_t end = q + 1; bool escaped = false;
            for (; end < document.size(); ++end) {
                if (!escaped && document[end] == '"') break;
                if (!escaped && document[end] == '\\') escaped = true; else escaped = false;
            }
            if (end < document.size()) { document.replace(q + 1, end - q - 1, JsonEscape(value)); return; }
        }
        p += needle.size();
    }
    size_t end = document.find_last_of('}');
    if (end == std::string::npos) return;
    size_t before = end;
    while (before > 0 && std::isspace(static_cast<unsigned char>(document[before - 1]))) --before;
    bool hasMember = before > 0 && document[before - 1] != '{';
    std::string addition = (hasMember ? ",\n" : "\n") + std::string("  \"") + key + "\": \"" + JsonEscape(value) + "\"\n";
    document.insert(before, addition);
}

void RemoveJsonString(std::string& document, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t p = document.find(needle);
    if (p == std::string::npos) return;
    size_t colon = SkipSpace(document, p + needle.size());
    size_t value = SkipSpace(document, colon + 1);
    if (colon >= document.size() || document[colon] != ':' || value >= document.size() || document[value] != '"') return;
    size_t end = value + 1;
    bool escaped = false;
    for (; end < document.size(); ++end) {
        if (!escaped && document[end] == '"') { ++end; break; }
        if (!escaped && document[end] == '\\') escaped = true; else escaped = false;
    }
    if (end > document.size()) return;

    size_t begin = document.rfind('\n', p);
    begin = begin == std::string::npos ? 0 : begin + 1;
    size_t after = SkipSpace(document, end);
    if (after < document.size() && document[after] == ',') {
        ++after;
        if (after < document.size() && document[after] == '\r') ++after;
        if (after < document.size() && document[after] == '\n') ++after;
        document.erase(begin, after - begin);
        return;
    }
    size_t before = begin;
    while (before > 0 && std::isspace(static_cast<unsigned char>(document[before - 1]))) --before;
    if (before > 0 && document[before - 1] == ',') begin = before - 1;
    document.erase(begin, end - begin);
}

void RemoveAllJsonStrings(std::string& document, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    while (document.find(needle) != std::string::npos) {
        const size_t sizeBefore = document.size();
        RemoveJsonString(document, key);
        if (document.size() == sizeBefore) break;
    }
}

bool FindJsonArray(const std::string& document, const char* key, size_t& begin, size_t& end) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = document.find(needle, p)) != std::string::npos) {
        const size_t colon = SkipSpace(document, p + needle.size());
        const size_t array = colon < document.size() && document[colon] == ':' ? SkipSpace(document, colon + 1) : document.size();
        if (array >= document.size() || document[array] != '[') { p += needle.size(); continue; }
        bool quoted = false, escaped = false;
        int depth = 0;
        for (size_t i = array; i < document.size(); ++i) {
            const char c = document[i];
            if (quoted) {
                if (!escaped && c == '\\') escaped = true;
                else if (!escaped && c == '"') quoted = false;
                else escaped = false;
                continue;
            }
            if (c == '"') { quoted = true; continue; }
            if (c == '/' && i + 1 < document.size() && document[i + 1] == '/') {
                i = document.find('\n', i + 2);
                if (i == std::string::npos) return false;
                continue;
            }
            if (c == '/' && i + 1 < document.size() && document[i + 1] == '*') {
                const size_t close = document.find("*/", i + 2);
                if (close == std::string::npos) return false;
                i = close + 1;
                continue;
            }
            if (c == '[') ++depth;
            else if (c == ']' && --depth == 0) { begin = array + 1; end = i; return true; }
        }
        return false;
    }
    return false;
}

bool FindJsonObjectEnd(const std::string& document, size_t begin, size_t limit, size_t& end) {
    if (begin >= limit || document[begin] != '{') return false;
    bool quoted = false, escaped = false;
    int depth = 0;
    for (size_t i = begin; i < limit; ++i) {
        const char c = document[i];
        if (quoted) {
            if (!escaped && c == '\\') escaped = true;
            else if (!escaped && c == '"') quoted = false;
            else escaped = false;
            continue;
        }
        if (c == '"') { quoted = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}' && --depth == 0) { end = i + 1; return true; }
    }
    return false;
}

bool ReadJsonStringArray(const std::string& document, const char* key, std::vector<std::wstring>& values) {
    values.clear();
    size_t begin = 0, end = 0;
    if (!FindJsonArray(document, key, begin, end)) return true;
    for (size_t p = begin; p < end;) {
        p = SkipSpace(document, p);
        if (p >= end) break;
        if (document[p] == ',') { ++p; continue; }
        if (document[p] != '"') return false;
        ++p;
        std::string raw;
        bool escaped = false, closed = false;
        for (; p < end; ++p) {
            const char c = document[p];
            if (!escaped && c == '"') { ++p; closed = true; break; }
            if (!escaped && c == '\\') { escaped = true; continue; }
            if (escaped) {
                if (c == 'n') raw.push_back('\n');
                else if (c == 'r') raw.push_back('\r');
                else if (c == 't') raw.push_back('\t');
                else raw.push_back(c);
                escaped = false;
            } else raw.push_back(c);
        }
        if (!closed) return false;
        values.push_back(WideFromUtf8(raw));
    }
    return true;
}

bool ReadJsonSourceArray(const std::string& document, std::vector<TargetSourceEntry>& values) {
    values.clear();
    size_t begin = 0, end = 0;
    if (!FindJsonArray(document, "sources", begin, end)) return true;
    for (size_t p = begin; p < end;) {
        p = SkipSpace(document, p);
        if (p >= end) break;
        if (document[p] == ',') { ++p; continue; }
        if (document[p] != '{') return false;
        size_t objectEnd = 0;
        if (!FindJsonObjectEnd(document, p, end, objectEnd)) return false;
        const std::string object = document.substr(p, objectEnd - p);
        std::wstring kind, path;
        if (!FindJsonString(object, "kind", kind) || !FindJsonString(object, "path", path) || path.empty()) return false;
        if (_wcsicmp(kind.c_str(), L"file") != 0 && _wcsicmp(kind.c_str(), L"folder") != 0) return false;
        values.push_back({path, _wcsicmp(kind.c_str(), L"folder") == 0});
        p = objectEnd;
    }
    return true;
}

bool IsSafeCmakeValue(const std::wstring& value) {
    if (Trim(value).empty()) return false;
    for (wchar_t c : value) {
        if (c == L'\r' || c == L'\n' || c == L'"' || c == L';' || c == L'$') return false;
    }
    return true;
}

std::wstring CmakePath(const std::wstring& value) {
    std::wstring result = Trim(value);
    for (wchar_t& c : result) if (c == L'\\') c = L'/';
    return result;
}

std::string CmakeQuoted(const std::wstring& value) {
    std::wstring normalized = CmakePath(value);
    std::string utf8 = Utf8FromWide(normalized);
    std::string result = "\"";
    for (char c : utf8) {
        if (c == '\\' || c == '"') result.push_back('\\');
        result.push_back(c);
    }
    return result + "\"";
}

bool ValidateCmakeTargetConfig(const CMakeTargetConfig& config, std::wstring& error) {
    error.clear();
    for (const TargetSourceEntry& source : config.sources) {
        const std::wstring path = CmakePath(source.path);
        if (!IsSafeCmakeValue(path) || !PathIsRelativeW(path.c_str())) { error = L"源文件或源文件夹必须是相对于 .ioc 指定 CMake 目录的路径，且不能包含不支持的字符。"; return false; }
    }
    const std::vector<std::wstring>* lists[] = {&config.includeDirectories, &config.compileDefinitions, &config.linkDirectories};
    const wchar_t* names[] = {L"头文件目录", L"编译宏", L"链接目录"};
    for (size_t i = 0; i < ARRAY_SIZE(lists); ++i) {
        for (const std::wstring& value : *lists[i]) {
            if (!IsSafeCmakeValue(value)) { error = std::wstring(names[i]) + L"包含不支持的字符。"; return false; }
            if (i != 1 && !PathIsRelativeW(CmakePath(value).c_str())) {
                error = std::wstring(names[i]) + L"必须是相对于 .ioc 指定 CMake 目录的路径。";
                return false;
            }
        }
    }
    return true;
}

void AppendJsonStringArray(std::string& document, const char* key, const std::vector<std::wstring>& values, bool trailingComma) {
    document += "  \"" + std::string(key) + "\": [";
    if (!values.empty()) document += "\n";
    for (size_t i = 0; i < values.size(); ++i) {
        document += "    \"" + JsonEscape(values[i]) + "\"";
        document += i + 1 == values.size() ? "\n" : ",\n";
    }
    document += trailingComma ? "  ],\n" : "  ]\n";
}

bool EnsureTargetConfigInclude(const WorkspaceInfo& info, std::wstring& error) {
    const std::wstring cmakeListsPath = JoinPath(info.toolchainRoot, L"CMakeLists.txt");
    std::string document;
    if (!ReadBytes(cmakeListsPath, document)) { error = L"无法读取 CMakeLists.txt：" + cmakeListsPath; return false; }
    if (document.find("PathConfiguratorProject.cmake") != std::string::npos) return true;
    const std::string include = "# PathConfigurator: 工程目标配置（由配置器维护）\ninclude(\"${CMAKE_CURRENT_LIST_DIR}/cmake/PathConfiguratorProject.cmake\")\n\n";
    const std::string insertionPoint = "# 用户自定义库搜索路径。";
    const size_t pos = document.find(insertionPoint);
    if (pos != std::string::npos) document.insert(pos, include);
    else document += "\n" + include;
    if (!WriteBytesAtomic(cmakeListsPath, document)) { error = L"无法更新 CMakeLists.txt：" + cmakeListsPath; return false; }
    return true;
}

// CubeMX 的 H7 CMSIS 头文件以 __GNUC__ 判断 GCC 风格属性，而 st-arm-clang
// 也会定义该宏。此模块仅在 CMake 实际选中 Clang 时抑制这一个兼容性警告。
// 必须在 add_executable 和 CubeMX 的 stm32cubemx 子目录之前 include，才能传递给所有目标。
bool EnsureCompilerCompatibilityModule(const WorkspaceInfo& info, std::wstring& error) {
    const std::wstring cmakeDirectory = JoinPath(info.toolchainRoot, L"cmake");
    const std::wstring modulePath = JoinPath(cmakeDirectory, L"PathConfiguratorCompilerCompat.cmake");
    if (!FolderExists(cmakeDirectory) && !CreateDirectoryW(cmakeDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"无法创建 CMake 模块目录：" + cmakeDirectory;
        return false;
    }
    const std::string module =
        "# PathConfigurator: STM32CubeMX/CMSIS 与 st-arm-clang 的兼容规则。\n"
        "# Clang 为兼容 GCC 定义 __GNUC__，但不支持 CMSIS 使用的 optimize(\"Os\") 属性。\n"
        "# 不修改 CubeMX 生成的 CMSIS 文件；仅对 Clang 关闭该兼容性警告。\n"
        "if(CMAKE_C_COMPILER_ID STREQUAL \"Clang\")\n"
        "  add_compile_options(-Wno-unknown-attributes)\n"
        "endif()\n";
    if (!WriteBytesAtomic(modulePath, module)) {
        error = L"无法写入 Clang 兼容模块：" + modulePath;
        return false;
    }

    const std::wstring cmakeListsPath = JoinPath(info.toolchainRoot, L"CMakeLists.txt");
    std::string document;
    if (!ReadBytes(cmakeListsPath, document)) {
        error = L"无法读取 CMakeLists.txt：" + cmakeListsPath;
        return false;
    }
    std::string lower = document;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t targetPos = lower.find("add_executable(");
    if (targetPos == std::string::npos) {
        error = L"未在 CMakeLists.txt 中找到 CubeMX 的 add_executable(...)，无法安全接入 Clang 兼容规则。";
        return false;
    }
    const std::string include =
        "# PathConfigurator: st-arm-clang 与 CubeMX CMSIS 的兼容规则\n"
        "include(\"${CMAKE_CURRENT_LIST_DIR}/cmake/PathConfiguratorCompilerCompat.cmake\")\n\n";
    const size_t includePos = document.find(include);
    if (includePos != std::string::npos && includePos < targetPos) return true;
    if (includePos != std::string::npos) {
        // 迁移配置器旧版本放在 add_executable() 后的接入行。
        document.erase(includePos, include.size());
        lower = document;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        targetPos = lower.find("add_executable(");
    } else if (document.find("PathConfiguratorCompilerCompat.cmake") != std::string::npos) {
        error = L"已找到自定义的 PathConfiguratorCompilerCompat.cmake 引用，但它不在 add_executable(...) 之前；请手动移动后重新运行配置器。";
        return false;
    }
    document.insert(targetPos, include);
    if (!WriteBytesAtomic(cmakeListsPath, document)) {
        error = L"无法更新 CMakeLists.txt：" + cmakeListsPath;
        return false;
    }
    return true;
}

std::wstring ReadLineValue(const std::string& data, const char* key) {
    size_t p = data.find(key);
    if (p == std::string::npos) return {};
    p += std::strlen(key);
    size_t e = data.find_first_of("\r\n", p);
    std::string v = data.substr(p, e == std::string::npos ? std::string::npos : e - p);
    return Trim(WideFromUtf8(v));
}

std::wstring DeriveSvdStem(const std::wstring& chip) {
    std::wstring upper = chip;
    for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
    size_t p = upper.find(L"STM32");
    if (p == std::wstring::npos) return {};
    p += 5;
    if (p + 4 > upper.size()) return {};
    return upper.substr(p - 5, 9);
}

std::wstring FindSvdCandidate(const std::wstring& dir, const std::wstring& stem) {
    if (dir.empty() || stem.empty()) return {};
    std::wstring direct = JoinPath(dir, stem + L".svd");
    if (FileExists(direct)) return direct;
    std::wstring common = JoinPath(dir, L"STMicroelectronics_CMSIS_SVD\\" + stem + L".svd");
    if (FileExists(common)) return common;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring result;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        std::wstring child = JoinPath(dir, fd.cFileName);
        result = FindSvdCandidate(child, stem);
        if (!result.empty()) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return result;
}

bool ReadRegistryString(HKEY key, const wchar_t* valueName, std::wstring& value) {
    value.clear();
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        return false;
    }
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type,
                         reinterpret_cast<LPBYTE>(buffer.data()), &bytes) != ERROR_SUCCESS) {
        return false;
    }
    value.assign(buffer.data());
    if (type == REG_EXPAND_SZ) {
        DWORD required = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (required > 0) {
            std::vector<wchar_t> expanded(required, L'\0');
            if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), required) > 0)
                value.assign(expanded.data());
        }
    }
    return !value.empty();
}

std::wstring CleanRegistryPath(std::wstring value) {
    value = Trim(value);
    while (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = Trim(value.substr(1, value.size() - 2));
    return NormalizePath(value);
}

bool ContainsInsensitive(const std::wstring& value, const wchar_t* needle) {
    std::wstring upper = value;
    std::wstring target = needle ? needle : L"";
    for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
    for (wchar_t& c : target) c = static_cast<wchar_t>(towupper(c));
    return !target.empty() && upper.find(target) != std::wstring::npos;
}

void AddRegistryRoot(std::vector<std::wstring>& roots, const std::wstring& rawPath) {
    std::wstring path = CleanRegistryPath(rawPath);
    if (!FolderExists(path)) return;
    for (const std::wstring& existing : roots)
        if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return;
    roots.push_back(path);
}

void CollectCubeCltRegistryRoots(std::vector<std::wstring>& roots, REGSAM view) {
    HKEY productKey = nullptr;
    const wchar_t* productPath = L"SOFTWARE\\STMicroelectronics\\STM32CubeCLT";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, productPath, 0, KEY_READ | view, &productKey) == ERROR_SUCCESS) {
        DWORD index = 0;
        wchar_t name[256]{};
        DWORD nameLength = ARRAY_SIZE(name);
        while (RegEnumKeyExW(productKey, index++, name, &nameLength, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY versionKey = nullptr;
            if (RegOpenKeyExW(productKey, name, 0, KEY_READ, &versionKey) == ERROR_SUCCESS) {
                std::wstring path;
                if (ReadRegistryString(versionKey, L"Path", path)) AddRegistryRoot(roots, path);
                RegCloseKey(versionKey);
            }
            nameLength = ARRAY_SIZE(name);
        }
        RegCloseKey(productKey);
    }

    HKEY uninstallKey = nullptr;
    const wchar_t* uninstallPath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, uninstallPath, 0, KEY_READ | view, &uninstallKey) != ERROR_SUCCESS)
        return;
    DWORD index = 0;
    wchar_t name[512]{};
    DWORD nameLength = ARRAY_SIZE(name);
    while (RegEnumKeyExW(uninstallKey, index++, name, &nameLength, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        HKEY item = nullptr;
        if (RegOpenKeyExW(uninstallKey, name, 0, KEY_READ, &item) == ERROR_SUCCESS) {
            std::wstring displayName, publisher, installLocation;
            ReadRegistryString(item, L"DisplayName", displayName);
            ReadRegistryString(item, L"Publisher", publisher);
            if (ContainsInsensitive(displayName, L"STM32CubeCLT") &&
                (publisher.empty() || ContainsInsensitive(publisher, L"STMicroelectronics"))) {
                if (ReadRegistryString(item, L"InstallLocation", installLocation))
                    AddRegistryRoot(roots, installLocation);
            }
            RegCloseKey(item);
        }
        nameLength = ARRAY_SIZE(name);
    }
    RegCloseKey(uninstallKey);
}

int DetectCubeCltToolsAtRoot(const std::wstring& root, ToolPaths& tools, std::wstring& report) {
    tools = {};
    report.clear();
    struct Candidate { const wchar_t* relative; const wchar_t* label; };
    const Candidate candidates[] = {
        {L"CMake\\bin\\cmake.exe", L"CMake"},
        {L"Ninja\\bin\\ninja.exe", L"Ninja"},
        {L"st-arm-clang\\bin\\starm-clang.exe", L"starm-clang"},
        {L"GNU-tools-for-STM32\\bin\\arm-none-eabi-gdb.exe", L"ARM GDB"}
    };
    int count = 0;
    for (const Candidate& candidate : candidates) {
        std::wstring path = JoinPath(root, candidate.relative);
        if (!FileExists(path)) continue;
        if (candidate.label == std::wstring(L"CMake")) tools.cmake = path;
        else if (candidate.label == std::wstring(L"Ninja")) tools.ninja = path;
        else if (candidate.label == std::wstring(L"starm-clang")) tools.starmClang = path;
        else tools.gdb = path;
        report += std::wstring(candidate.label) + L"：" + path + L"\r\n";
        ++count;
    }
    return count;
}

bool IsOpenOcdConfigNameValid(const std::wstring& name) {
    if (name.empty()) return true;
    for (wchar_t c : name) {
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'_' || c == L'-')) {
            return false;
        }
    }
    return true;
}

bool IsOpenOcdTargetFileValid(const std::wstring& name) {
    constexpr size_t extensionLength = 4;
    if (name.size() <= extensionLength || _wcsicmp(name.c_str() + name.size() - extensionLength, L".cfg") != 0)
        return false;
    return IsOpenOcdConfigNameValid(name.substr(0, name.size() - extensionLength));
}

std::wstring DeriveOpenOcdFamily(const std::wstring& chipType) {
    std::wstring upper = chipType;
    for (wchar_t& c : upper) c = static_cast<wchar_t>(towupper(c));
    size_t pos = upper.find(L"STM32");
    if (pos == std::wstring::npos || pos + 6 >= upper.size()) return {};
    pos += 5;
    if (!iswalpha(upper[pos])) return {};
    std::wstring family = L"stm32";
    family.push_back(static_cast<wchar_t>(towlower(upper[pos++])));
    while (pos < upper.size() && !iswdigit(upper[pos])) ++pos;
    if (pos == upper.size()) return {};
    family.push_back(upper[pos]);
    return family;
}

} // namespace

std::wstring Trim(const std::wstring& value) {
    size_t a = 0, b = value.size();
    while (a < b && iswspace(value[a])) ++a;
    while (b > a && iswspace(value[b - 1])) --b;
    return value.substr(a, b - a);
}

std::wstring NormalizePath(const std::wstring& value) {
    std::wstring p = Trim(value);
    if (p.empty()) return {};
    for (wchar_t& c : p) if (c == L'/') c = L'\\';
    wchar_t full[MAX_PATH * 4]{};
    DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(ARRAY_SIZE(full)), full, nullptr);
    if (n > 0 && n < ARRAY_SIZE(full)) p.assign(full, n);
    return p;
}

bool FileExists(const std::wstring& path) { return !path.empty() && PathFileExistsW(path.c_str()) && !(GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY); }
bool FolderExists(const std::wstring& path) { return !path.empty() && PathFileExistsW(path.c_str()) && (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring JoinPath(const std::wstring& left, const std::wstring& right) { if (left.empty()) return right; if (right.empty()) return left; return left.back() == L'\\' ? left + right : left + L'\\' + right; }
std::wstring GetParentPath(const std::wstring& path) {
    std::wstring p = NormalizePath(path);
    if (p.empty() || PathIsRootW(p.c_str())) return p;
    size_t n = p.find_last_of(L"\\/");
    if (n == std::wstring::npos) return {};
    // 保留 "D:\\"，避免将盘符根目录降为 "D:" 后又被解析回当前工作目录。
    if (n == 2 && p.size() >= 3 && p[1] == L':') return p.substr(0, 3);
    return p.substr(0, n);
}
std::wstring FileName(const std::wstring& path) { std::wstring p = NormalizePath(path); size_t n = p.find_last_of(L"\\/"); return n == std::wstring::npos ? p : p.substr(n + 1); }

bool MakeToolchainRelativePath(const WorkspaceInfo& info, const std::wstring& selectedPath,
                               bool isDirectory, std::wstring& relativePath) {
    relativePath.clear();
    const std::wstring full = NormalizePath(selectedPath);
    if (info.toolchainRoot.empty() || full.empty()) return false;
    wchar_t relative[MAX_PATH * 4]{};
    const DWORD attributes = isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!PathRelativePathToW(relative, info.toolchainRoot.c_str(), FILE_ATTRIBUTE_DIRECTORY,
                             full.c_str(), attributes)) return false;
    relativePath = relative;
    while (relativePath.rfind(L".\\", 0) == 0) relativePath.erase(0, 2);
    for (wchar_t& c : relativePath) if (c == L'\\') c = L'/';
    return !relativePath.empty() && PathIsRelativeW(relativePath.c_str());
}

std::wstring FindIocFile(const std::wstring& root) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinPath(root, L"*.ioc").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring result = JoinPath(root, fd.cFileName);
    FindClose(h);
    return result;
}

std::wstring ReadIocSetting(const std::wstring& ioc, const char* key) {
    std::string data;
    return (!ioc.empty() && ReadBytes(ioc, data)) ? ReadLineValue(data, key) : std::wstring();
}

std::wstring ResolveToolchainDirectory(const std::wstring& root) {
    std::wstring ioc = FindIocFile(root);
    if (ioc.empty()) return {};
    std::wstring location = ReadIocSetting(ioc, "ProjectManager.ToolChainLocation=");
    // CubeMX permits an empty location when CMake files are generated in root.
    std::wstring candidate = location.empty() ? NormalizePath(root) : NormalizePath(location);
    if (!location.empty() && location.find(L':') == std::wstring::npos && location[0] != L'\\')
        candidate = NormalizePath(JoinPath(root, location));
    if (FolderExists(candidate) && FileExists(JoinPath(candidate, L"CMakeLists.txt"))) return candidate;
    return {};
}

std::wstring FindWorkspaceRoot(const std::wstring& start) {
    std::wstring current = NormalizePath(start);
    if (FileExists(current)) current = GetParentPath(current);
    while (!current.empty()) {
        std::wstring ioc = FindIocFile(current);
        if (!ioc.empty() && _wcsicmp(ReadIocSetting(ioc, "ProjectManager.TargetToolchain=").c_str(), L"CMake") == 0 && !ResolveToolchainDirectory(current).empty())
            return current;
        std::wstring parent = GetParentPath(current);
        if (parent == current) break;
        current = parent;
    }
    return {};
}

std::wstring RelativeToolchainLocation(const std::wstring& root, const std::wstring& toolchain) {
    std::wstring base = NormalizePath(root);
    std::wstring target = NormalizePath(toolchain);
    if (_wcsicmp(base.c_str(), target.c_str()) == 0) return L".";
    if (target.size() > base.size() && _wcsnicmp(target.c_str(), base.c_str(), base.size()) == 0 && target[base.size()] == L'\\') {
        std::wstring relative = target.substr(base.size() + 1);
        for (wchar_t& c : relative) if (c == L'\\') c = L'/';
        return relative;
    }
    return target;
}

std::wstring SlashPath(std::wstring value) {
    for (wchar_t& c : value) if (c == L'\\') c = L'/';
    return value;
}

bool LoadWorkspace(const std::wstring& root, WorkspaceInfo& info, std::wstring& error) {
    info = {};
    info.root = NormalizePath(root);
    if (!FolderExists(info.root)) { error = L"工程目录不存在：" + info.root; return false; }
    std::wstring iocPath = FindIocFile(info.root);
    if (iocPath.empty()) { error = L"未找到 .ioc 文件，无法确认这是 STM32CubeMX 工程。"; return false; }
    std::wstring targetToolchain = ReadIocSetting(iocPath, "ProjectManager.TargetToolchain=");
    if (_wcsicmp(targetToolchain.c_str(), L"CMake") != 0) {
        error = L"当前工程的 CubeMX TargetToolchain 不是 CMake（当前值：" + (targetToolchain.empty() ? L"未设置" : targetToolchain) + L"）。";
        return false;
    }
    info.toolchainRoot = ResolveToolchainDirectory(info.root);
    if (info.toolchainRoot.empty()) { error = L"项目尚未生成 CMake 代码，未找到由 ProjectManager.ToolChainLocation 指定的目录或 CMakeLists.txt。"; return false; }
    info.toolchainLocation = RelativeToolchainLocation(info.root, info.toolchainRoot);
    info.settingsPath = JoinPath(info.root, L".vscode\\settings.json");
    info.examplePath = JoinPath(info.root, L".vscode\\settings.example.json");
    info.presetsPath = JoinPath(info.toolchainRoot, L"CMakeUserPresets.json");
    info.cmakeTargetConfigPath = JoinPath(info.toolchainRoot, L"project-config.json");
    info.cmakeTargetModulePath = JoinPath(info.toolchainRoot, L"cmake\\PathConfiguratorProject.cmake");
    info.hasExample = FileExists(info.examplePath);
    std::wstring cmakeLists = JoinPath(info.toolchainRoot, L"CMakeLists.txt");
    std::string data;
    if (ReadBytes(cmakeLists, data)) {
        std::string lower = data; for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t p = lower.find("set(cmake_project_name");
        if (p != std::string::npos) {
            size_t a = data.find_first_not_of(" \t\r\n", p + 22);
            size_t b = data.find_first_of(" \t\r\n)", a);
            if (a != std::string::npos) info.projectName = WideFromUtf8(data.substr(a, b - a));
        }
    }
    if (info.projectName.empty()) info.projectName = FileName(info.root);
    if (ReadBytes(iocPath, data)) {
        std::wstring cpn = ReadLineValue(data, "Mcu.CPN=");
        if (cpn.empty()) cpn = ReadLineValue(data, "Mcu.Name=");
        info.chipType = cpn;
    }
    if (info.chipType.empty()) info.chipType = L"Unknown";
    std::wstring parent = GetParentPath(info.root);
    info.projectLabel = FileName(parent).empty() ? FileName(info.root) : FileName(parent) + L"\\" + FileName(info.root);
    return true;
}

bool LoadSettings(const std::wstring& path, ToolPaths& tools, std::wstring& projectName, std::wstring& chipType,
                 std::wstring& svd, std::string& document) {
    document.clear(); tools = {}; projectName.clear(); chipType.clear(); svd.clear();
    if (!FileExists(path) || !ReadBytes(path, document)) return false;
    std::wstring* values[] = {&tools.cmake, &tools.ninja, &tools.starmClang, &tools.gdb, &tools.openocd, &tools.svd};
    const char* keys[] = {"CustomCfg.cmakePath", "CustomCfg.ninjaPath", "CustomCfg.starmClangPath", "CustomCfg.gdbPath", "CustomCfg.openocdPath", "CustomCfg.svdFile"};
    for (size_t i = 0; i < ARRAY_SIZE(keys); ++i)
        FindJsonString(document, keys[i], *values[i]);
    FindJsonString(document, "CustomCfg.openocd.interface", tools.openocdInterface);
    FindJsonString(document, "CustomCfg.openocd.target", tools.openocdTarget);
    if (tools.openocdInterface.empty()) tools.openocdInterface = L"cmsis-dap";
    svd = tools.svd;
    FindJsonString(document, "CustomCfg.projectName", projectName);
    FindJsonString(document, "CustomCfg.device", chipType);
    if (chipType.empty()) {
        FindJsonString(document, "CustomCfg.chipType", chipType);
    }
    SanitizeToolPaths(tools);
    svd = tools.svd;
    return true;
}

ValidationResult ValidateTools(const ToolPaths& tools, bool requireSvd) {
    ValidationResult r; r.ok = true;
    struct Item { const wchar_t* label; const wchar_t* fileName; const std::wstring* value; } items[] = {
        {L"CMake", L"cmake.exe", &tools.cmake}, {L"Ninja", L"ninja.exe", &tools.ninja},
        {L"starm-clang", L"starm-clang.exe", &tools.starmClang},
        {L"GDB", L"arm-none-eabi-gdb.exe", &tools.gdb}, {L"OpenOCD", L"openocd.exe", &tools.openocd}
    };
    for (const Item& item : items) {
        const std::wstring value = item.value ? *item.value : L"";
        const bool ok = FileExists(value) && _wcsicmp(FileName(value).c_str(), item.fileName) == 0;
        if (!ok) { r.ok = false; r.message += std::wstring(item.label) + L"：" + (item.value ? *item.value : L"") + L"\r\n"; }
    }
    struct OpenOcdConfig { const wchar_t* label; const std::wstring* name; const wchar_t* folder; bool appendExtension; } openOcdConfigs[] = {
        {L"OpenOCD interface", &tools.openocdInterface, L"interface", true},
        {L"OpenOCD target", &tools.openocdTarget, L"target", false}
    };
    std::wstring detectedScripts;
    const bool canCheckConfigs = FileExists(tools.openocd) && DetectOpenOcdScripts(tools.openocd, detectedScripts);
    for (const OpenOcdConfig& config : openOcdConfigs) {
        if (config.name->empty()) continue; // 可选配置留空时不参与校验。
        const bool valid = config.appendExtension ? IsOpenOcdConfigNameValid(*config.name) : IsOpenOcdTargetFileValid(*config.name);
        if (!valid) {
            r.ok = false;
            r.message += std::wstring(config.label) + (config.appendExtension ? L" 只能包含英文字母、数字、下划线或连字符。\r\n" : L" 必须是 .cfg 文件名，且仅可包含英文字母、数字、下划线或连字符。\r\n");
        } else if (canCheckConfigs && !FileExists(JoinPath(JoinPath(detectedScripts, config.folder), *config.name + (config.appendExtension ? L".cfg" : L"")))) {
            r.ok = false;
            r.message += std::wstring(config.label) + L" 对应的 .cfg 文件不存在：" + *config.name + L"\r\n";
        }
    }
    const std::wstring svdName = FileName(tools.svd);
    const bool validSvd = FileExists(tools.svd) && svdName.size() > 4 &&
        _wcsicmp(svdName.c_str() + svdName.size() - 4, L".svd") == 0;
    if (requireSvd && !validSvd) { r.ok = false; r.message += L"SVD：" + tools.svd + L"\r\n"; }
    if (r.ok) r.message = L"所有配置路径有效。";
    return r;
}

bool DetectCubeClt(const std::wstring& selectedPath, ToolPaths& tools, std::wstring& cubeRoot, std::wstring& report) {
    std::wstring current = NormalizePath(selectedPath);
    if (FileExists(current)) current = GetParentPath(current);
    cubeRoot.clear(); report.clear();
    while (!current.empty()) {
        if (FileExists(JoinPath(current, L"STM32CubeCLT_metadata.bat"))) { cubeRoot = current; break; }
        std::wstring parent = GetParentPath(current); if (parent == current) break; current = parent;
    }
    if (cubeRoot.empty()) return false;
    const std::wstring candidates[][2] = {
        {L"CMake\\bin\\cmake.exe", L"cmake"}, {L"Ninja\\bin\\ninja.exe", L"ninja"},
        {L"st-arm-clang\\bin\\starm-clang.exe", L"starm-clang"},
        {L"GNU-tools-for-STM32\\bin\\arm-none-eabi-gdb.exe", L"gdb"}
    };
    for (const auto& candidate : candidates) {
        std::wstring value = JoinPath(cubeRoot, candidate[0]);
        if (FileExists(value)) {
            if (candidate[1] == L"cmake") tools.cmake = value;
            else if (candidate[1] == L"ninja") tools.ninja = value;
            else if (candidate[1] == L"starm-clang") tools.starmClang = value;
            else tools.gdb = value;
            report += std::wstring(candidate[1]) + L" = " + value + L"\r\n";
        }
    }
    return true;
}

bool DetectCubeCltFromRegistry(ToolPaths& tools, std::wstring& cubeRoot, std::wstring& report) {
    tools = {};
    cubeRoot.clear();
    report.clear();

    std::vector<std::wstring> roots;
    CollectCubeCltRegistryRoots(roots, KEY_WOW64_32KEY);
    CollectCubeCltRegistryRoots(roots, KEY_WOW64_64KEY);
    if (roots.empty()) return false;

    int bestCount = 0;
    ToolPaths bestTools;
    std::wstring bestReport;
    for (const std::wstring& root : roots) {
        ToolPaths candidateTools;
        std::wstring candidateReport;
        int count = DetectCubeCltToolsAtRoot(root, candidateTools, candidateReport);
        if (count > bestCount) {
            bestCount = count;
            cubeRoot = root;
            bestTools = candidateTools;
            bestReport = candidateReport;
        }
    }
    if (bestCount == 0) {
        cubeRoot.clear();
        return false;
    }
    tools = bestTools;
    report = bestReport;
    return true;
}

bool DetectOpenOcdScripts(const std::wstring& openocdPath, std::wstring& scriptsPath) {
    if (!FileExists(openocdPath)) return false;
    std::wstring exeDir = GetParentPath(openocdPath), package = GetParentPath(exeDir);
    const std::wstring candidates[] = {JoinPath(exeDir, L"scripts"), JoinPath(package, L"scripts"), JoinPath(package, L"openocd\\scripts"), JoinPath(package, L"share\\openocd\\scripts")};
    for (const auto& candidate : candidates) if (FolderExists(candidate)) { scriptsPath = candidate; return true; }
    return false;
}

std::wstring GetUserDefaultSettingsPath() {
    DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (size == 0) return {};
    std::wstring localAppData(static_cast<size_t>(size), L'\0');
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), size) == 0) return {};
    localAppData.resize(wcslen(localAppData.c_str()));
    return JoinPath(JoinPath(localAppData, L"PathConfigurator"), L"user-settings.json");
}

void SanitizeToolPaths(ToolPaths& tools) {
    struct ExpectedFile { std::wstring* path; const wchar_t* name; } executablePaths[] = {
        {&tools.cmake, L"cmake.exe"}, {&tools.ninja, L"ninja.exe"},
        {&tools.starmClang, L"starm-clang.exe"}, {&tools.gdb, L"arm-none-eabi-gdb.exe"},
        {&tools.openocd, L"openocd.exe"}};
    for (const ExpectedFile& item : executablePaths) {
        std::wstring& path = *item.path;
        path = NormalizePath(path);
        if (!FileExists(path) || _wcsicmp(FileName(path).c_str(), item.name) != 0) path.clear();
    }
    tools.svd = NormalizePath(tools.svd);
    const std::wstring svdName = FileName(tools.svd);
    const bool svdExtensionValid = svdName.size() > 4 && _wcsicmp(svdName.c_str() + svdName.size() - 4, L".svd") == 0;
    if (!FileExists(tools.svd) || !svdExtensionValid) tools.svd.clear();

    tools.openocdInterface = Trim(tools.openocdInterface);
    tools.openocdTarget = Trim(tools.openocdTarget);
    std::wstring scripts;
    if (!DetectOpenOcdScripts(tools.openocd, scripts)) {
        tools.openocdInterface.clear();
        tools.openocdTarget.clear();
        return;
    }
    if (!tools.openocdInterface.empty() &&
        (!IsOpenOcdConfigNameValid(tools.openocdInterface) ||
         !FileExists(JoinPath(scripts, L"interface\\" + tools.openocdInterface + L".cfg")))) {
        tools.openocdInterface.clear();
    }
    if (!tools.openocdTarget.empty() &&
        (!IsOpenOcdTargetFileValid(tools.openocdTarget) ||
         !FileExists(JoinPath(scripts, L"target\\" + tools.openocdTarget)))) {
        tools.openocdTarget.clear();
    }
}

bool LoadUserDefaultSettings(ToolPaths& tools, std::wstring& error) {
    tools = {};
    error.clear();
    const std::wstring path = GetUserDefaultSettingsPath();
    if (path.empty()) { error = L"无法获取 %LOCALAPPDATA% 路径。"; return false; }
    if (!FileExists(path)) { error = L"默认配置不存在：" + path; return false; }
    std::string document;
    if (!ReadBytes(path, document)) { error = L"无法读取默认配置：" + path; return false; }
    std::wstring* values[] = {
        &tools.cmake, &tools.ninja, &tools.starmClang, &tools.gdb, &tools.openocd, &tools.openocdInterface};
    const char* keys[] = {
        "cmakePath", "ninjaPath", "starmClangPath", "gdbPath", "openocdPath", "openocdInterface"};
    for (size_t i = 0; i < ARRAY_SIZE(keys); ++i)
        FindJsonString(document, keys[i], *values[i]);
    SanitizeToolPaths(tools);
    // user-settings.json is cross-project; never restore project-bound values.
    tools.svd.clear();
    tools.openocdTarget.clear();
    return true;
}

bool WriteUserDefaultSettings(const ToolPaths& configuredTools, std::wstring& error) {
    error.clear();
    const std::wstring path = GetUserDefaultSettingsPath();
    if (path.empty()) { error = L"无法获取 %LOCALAPPDATA% 路径。"; return false; }
    const std::wstring directory = GetParentPath(path);
    if (!FolderExists(directory) && !CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"无法创建默认配置目录：" + directory;
        return false;
    }
    ToolPaths tools = configuredTools;
    SanitizeToolPaths(tools);
    std::string document = "{\n  \"version\": 1,\n";
    const std::pair<const char*, const std::wstring*> values[] = {
        {"cmakePath", &tools.cmake}, {"ninjaPath", &tools.ninja}, {"starmClangPath", &tools.starmClang},
        {"gdbPath", &tools.gdb}, {"openocdPath", &tools.openocd}, {"openocdInterface", &tools.openocdInterface}};
    for (size_t i = 0; i < ARRAY_SIZE(values); ++i) {
        document += "  \"" + std::string(values[i].first) + "\": \"" + JsonEscape(*values[i].second) + "\"";
        document += i + 1 == ARRAY_SIZE(values) ? "\n" : ",\n";
    }
    document += "}\n";
    if (!WriteBytesAtomic(path, document)) { error = L"无法写入默认配置：" + path; return false; }
    return true;
}

std::wstring FindOpenOcdTargetForChip(const std::wstring& openocdPath, const std::wstring& chipType) {
    std::wstring scripts;
    const std::wstring family = DeriveOpenOcdFamily(chipType);
    if (family.empty() || !DetectOpenOcdScripts(openocdPath, scripts)) return {};
    const std::wstring targetFolder = JoinPath(scripts, L"target");
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(JoinPath(targetFolder, L"*.cfg").c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return {};

    std::wstring best;
    int bestScore = -1;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring candidate = data.cFileName;
        std::wstring lower = candidate;
        for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
        if (lower.rfind(family, 0) != 0) continue;

        const std::wstring exact = family + L"x.cfg";
        const int score = lower == exact ? 2 : 1;
        if (score > bestScore || (score == bestScore && (best.empty() || candidate.size() < best.size())) ||
            (score == bestScore && candidate.size() == best.size() && _wcsicmp(candidate.c_str(), best.c_str()) < 0)) {
            best = candidate;
            bestScore = score;
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);
    return best;
}

std::wstring FindSvdForChip(const std::wstring& root, const std::wstring& chipType) {
    std::wstring stem = DeriveSvdStem(chipType);
    if (stem.empty()) return {};
    return FindSvdCandidate(root, stem);
}

bool LoadCMakeTargetConfig(const WorkspaceInfo& info, CMakeTargetConfig& config, std::wstring& error) {
    config = {};
    error.clear();
    if (!FileExists(info.cmakeTargetConfigPath)) return true;
    std::string document;
    if (!ReadBytes(info.cmakeTargetConfigPath, document)) {
        error = L"无法读取工程 CMake 配置：" + info.cmakeTargetConfigPath;
        return false;
    }
    if (!ReadJsonSourceArray(document, config.sources) ||
        !ReadJsonStringArray(document, "includeDirectories", config.includeDirectories) ||
        !ReadJsonStringArray(document, "compileDefinitions", config.compileDefinitions) ||
        !ReadJsonStringArray(document, "linkDirectories", config.linkDirectories) ||
        !ValidateCmakeTargetConfig(config, error)) {
        if (error.empty()) error = L"工程 CMake 配置的 JSON 格式无效：" + info.cmakeTargetConfigPath;
        return false;
    }
    return true;
}

bool WriteCMakeTargetConfig(const WorkspaceInfo& info, const CMakeTargetConfig& config, std::wstring& error) {
    if (!ValidateCmakeTargetConfig(config, error)) return false;
    if (!FolderExists(info.toolchainRoot)) {
        error = L"CMake 目录不存在：" + info.toolchainRoot;
        return false;
    }

    std::string json = "{\n  \"version\": 1,\n  \"sources\": [";
    if (!config.sources.empty()) json += "\n";
    for (size_t i = 0; i < config.sources.size(); ++i) {
        const TargetSourceEntry& source = config.sources[i];
        json += "    {\"kind\": \"" + std::string(source.isFolder ? "folder" : "file") + "\", \"path\": \"" + JsonEscape(CmakePath(source.path)) + "\"}";
        json += i + 1 == config.sources.size() ? "\n" : ",\n";
    }
    json += "  ],\n";
    AppendJsonStringArray(json, "includeDirectories", config.includeDirectories, true);
    AppendJsonStringArray(json, "compileDefinitions", config.compileDefinitions, true);
    AppendJsonStringArray(json, "linkDirectories", config.linkDirectories, false);
    json += "}\n";
    if (!WriteBytesAtomic(info.cmakeTargetConfigPath, json)) {
        error = L"无法写入工程 CMake 配置：" + info.cmakeTargetConfigPath;
        return false;
    }

    const std::wstring cmakeDirectory = GetParentPath(info.cmakeTargetModulePath);
    if (!FolderExists(cmakeDirectory) && !CreateDirectoryW(cmakeDirectory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = L"无法创建 CMake 模块目录：" + cmakeDirectory;
        return false;
    }
    std::string cmake =
        "# 由 STM32 PathConfigurator 根据 project-config.json 生成。\n"
        "# 源文件夹使用 GLOB_RECURSE + CONFIGURE_DEPENDS；新增/删除源文件时 CMake 会重新检查。\n"
        "set(PathConfiguratorProject_SOURCE_FILES)\n\n";
    for (size_t i = 0; i < config.sources.size(); ++i) {
        const TargetSourceEntry& source = config.sources[i];
        const std::string base = "${CMAKE_CURRENT_LIST_DIR}/../" + Utf8FromWide(CmakePath(source.path));
        if (!source.isFolder) {
            cmake += "list(APPEND PathConfiguratorProject_SOURCE_FILES \"" + base + "\")\n";
            continue;
        }
        const std::string variable = "PathConfiguratorProject_FOLDER_" + std::to_string(i);
        cmake += "file(GLOB_RECURSE " + variable + " CONFIGURE_DEPENDS\n";
        // Windows 文件系统大小写不敏感，*.s 已能匹配 .s 与 .S，避免同一汇编文件被加入两次。
        const char* extensions[] = {"c", "cc", "cp", "cpp", "cxx", "s", "asm"};
        for (const char* extension : extensions)
            cmake += "    \"" + base + "/*." + extension + "\"\n";
        cmake += ")\nlist(APPEND PathConfiguratorProject_SOURCE_FILES ${" + variable + "})\n";
    }
    if (!config.sources.empty()) cmake += "\n";
    if (!config.sources.empty()) {
        cmake += "if(PathConfiguratorProject_SOURCE_FILES)\n  target_sources(${CMAKE_PROJECT_NAME} PRIVATE ${PathConfiguratorProject_SOURCE_FILES})\nendif()\n\n";
    }
    if (!config.includeDirectories.empty()) {
        cmake += "target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE\n";
        for (const std::wstring& value : config.includeDirectories)
            cmake += "  \"${CMAKE_CURRENT_LIST_DIR}/../" + Utf8FromWide(CmakePath(value)) + "\"\n";
        cmake += ")\n\n";
    }
    if (!config.compileDefinitions.empty()) {
        cmake += "target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE\n";
        for (const std::wstring& value : config.compileDefinitions)
            cmake += "  " + CmakeQuoted(value) + "\n";
        cmake += ")\n\n";
    }
    if (!config.linkDirectories.empty()) {
        cmake += "target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE\n";
        for (const std::wstring& value : config.linkDirectories)
            cmake += "  \"${CMAKE_CURRENT_LIST_DIR}/../" + Utf8FromWide(CmakePath(value)) + "\"\n";
        cmake += ")\n";
    }
    if (!WriteBytesAtomic(info.cmakeTargetModulePath, cmake)) {
        error = L"无法写入 CMake 目标模块：" + info.cmakeTargetModulePath;
        return false;
    }
    return EnsureTargetConfigInclude(info, error);
}

bool WriteConfiguration(const WorkspaceInfo& info, const ToolPaths& tools, const std::wstring& projectName,
                        const std::wstring& chipType, const std::wstring& svd, bool fromExample, std::wstring& error) {
    std::wstring vscode = JoinPath(info.root, L".vscode");
    if (!FolderExists(vscode)) CreateDirectoryW(vscode.c_str(), nullptr);
    // 兼容规则属于工程构建规则并应提交 Git，不能依赖每位开发者各自的本机预设。
    // 先验证并接入它，避免无法安全修改 CMakeLists.txt 时仍写入本机配置。
    if (!EnsureCompilerCompatibilityModule(info, error)) return false;
    std::string settings;
    if (fromExample && FileExists(info.examplePath)) {
        if (!ReadBytes(info.examplePath, settings)) { error = L"无法读取 settings.example.json"; return false; }
    } else if (FileExists(info.settingsPath)) {
        if (!ReadBytes(info.settingsPath, settings)) { error = L"无法读取 settings.json"; return false; }
    } else settings = "{\n}\n";
    // CMake Tools does not expand ${config:...} in its own settings.  Keep
    // CustomCfg.toolchainLocation for tasks/debugging, but write the CMake
    // source directory as a directly resolvable VS Code workspace path.
    const std::wstring cmakeToolchain = PathIsRelativeW(info.toolchainLocation.c_str())
        ? L"${workspaceFolder}/" + SlashPath(info.toolchainLocation)
        : SlashPath(info.toolchainLocation);
    const std::wstring cmakeBuild = cmakeToolchain + L"/build/${buildType}";
    const std::wstring cmakeToolchainFile = cmakeToolchain + L"/cmake/starm-clang.cmake";
    const std::wstring cmakeCompileCommands = cmakeToolchain + L"/build/Debug/compile_commands.json";
    const std::wstring cmakeEnvironmentPath = SlashPath(GetParentPath(tools.ninja)) + L";" +
        SlashPath(GetParentPath(tools.starmClang)) + L";" + SlashPath(GetParentPath(tools.gdb)) + L";${env:PATH}";
    struct Setting { const char* key; const std::wstring* value; } values[] = {
        {"CustomCfg.toolchainLocation", &info.toolchainLocation},
        {"CustomCfg.cmakePath", &tools.cmake}, {"cmake.cmakePath", &tools.cmake}, {"CustomCfg.ninjaPath", &tools.ninja},
        {"CustomCfg.starmClangPath", &tools.starmClang}, {"CustomCfg.gdbPath", &tools.gdb}, {"CustomCfg.openocdPath", &tools.openocd},
        {"CustomCfg.openocd.interface", &tools.openocdInterface}, {"CustomCfg.openocd.target", &tools.openocdTarget},
        {"CustomCfg.projectName", &projectName}, {"CustomCfg.device", &chipType}, {"CustomCfg.chipType", &chipType},
        {"CustomCfg.svdFile", &svd},
        {"cmake.sourceDirectory", &cmakeToolchain}, {"cmake.buildDirectory", &cmakeBuild}, {"CMAKE_TOOLCHAIN_FILE", &cmakeToolchainFile},
        {"C_Cpp.default.compileCommands", &cmakeCompileCommands}
    };
    RemoveAllJsonStrings(settings, "CustomCfg.openocdScripts");
    // CubeMX 的 starm-clang.cmake 通过 PATH 查找 starm-clang，并不读取这个缓存变量。
    // 清理旧模板或已有 settings 中的值，避免 CMake 报“变量未使用”。
    RemoveAllJsonStrings(settings, "STARM_CLANG_PATH");
    for (const auto& item : values) ReplaceJsonString(settings, item.key, *item.value);
    ReplaceJsonString(settings, "CMAKE_MAKE_PROGRAM", tools.ninja);
    ReplaceJsonString(settings, "cmake.useCMakePresets", L"auto");
    ReplaceJsonString(settings, "PATH", cmakeEnvironmentPath);
    if (!WriteBytesAtomic(info.settingsPath, settings)) { error = L"写入 .vscode/settings.json 失败"; return false; }
    std::string presets = "{\n  \"version\": 3,\n  \"configurePresets\": [\n";
    const std::wstring presetPath = SlashPath(GetParentPath(tools.ninja)) + L";" +
        SlashPath(GetParentPath(tools.starmClang)) + L";" + SlashPath(GetParentPath(tools.gdb)) + L";$penv{PATH}";
    for (int i = 0; i < 2; ++i) {
        presets += "    {\"name\": \"" + std::string(i ? "Release-Local" : "Debug-Local") + "\", \"inherits\": \"" + std::string(i ? "Release" : "Debug") + "\", \"binaryDir\": \"${sourceDir}/build/" + std::string(i ? "Release" : "Debug") + "\", \"cacheVariables\": {\"CMAKE_MAKE_PROGRAM\": \"" + JsonEscape(tools.ninja) + "\"}, \"environment\": {\"PATH\": \"" + JsonEscape(presetPath) + "\"}}";
        presets += i ? "\n" : ",\n";
    }
    presets += "  ],\n  \"buildPresets\": [{\"name\": \"Debug-Local\", \"configurePreset\": \"Debug-Local\"}, {\"name\": \"Release-Local\", \"configurePreset\": \"Release-Local\"}]\n}\n";
    if (!WriteBytesAtomic(info.presetsPath, presets)) { error = L"写入 CMakeUserPresets.json 失败：" + info.presetsPath; return false; }
    return true;
}

} // namespace pathconfig
