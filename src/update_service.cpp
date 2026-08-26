#include "core.hpp"
#include "update_service.hpp"

#include <cwctype>
#include <limits>
#include <string>

#include <winhttp.h>

namespace pathconfig::update {
namespace {

constexpr wchar_t kReleasePage[] = L"https://github.com/Name-CK/PathConfigurator/releases";

bool Utf8ToWide(const std::string& value, std::wstring& result) {
    if (value.empty()) {
        result.clear();
        return true;
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return false;
    result.resize(static_cast<size_t>(length));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length) == length;
}

bool ExtractJsonString(const std::string& json, const char* key, std::string& value) {
    const std::string marker = std::string("\"") + key + "\"";
    const size_t keyPosition = json.find(marker);
    if (keyPosition == std::string::npos) return false;
    const size_t colon = json.find(':', keyPosition + marker.size());
    if (colon == std::string::npos) return false;
    const size_t quote = json.find('"', colon + 1);
    if (quote == std::string::npos) return false;
    value.clear();
    bool escaped = false;
    for (size_t i = quote + 1; i < json.size(); ++i) {
        const char current = json[i];
        if (escaped) {
            if (current == '"' || current == '\\' || current == '/') value.push_back(current);
            else if (current == 'n') value.push_back('\n');
            else if (current == 'r') value.push_back('\r');
            else if (current == 't') value.push_back('\t');
            else value.push_back(current);
            escaped = false;
            continue;
        }
        if (current == '\\') { escaped = true; continue; }
        if (current == '"') return true;
        value.push_back(current);
    }
    return false;
}

struct VersionParts { int value[3]{}; };

bool ParseVersion(const std::wstring& text, VersionParts& version) {
    version = {};
    size_t position = 0;
    while (position < text.size() && !iswdigit(text[position])) ++position;
    int component = 0;
    while (position < text.size() && component < 3) {
        while (position < text.size() && !iswdigit(text[position])) ++position;
        if (position == text.size()) break;
        int number = 0;
        while (position < text.size() && iswdigit(text[position])) {
            const int digit = text[position++] - L'0';
            number = number <= (std::numeric_limits<int>::max() - digit) / 10
                ? number * 10 + digit : std::numeric_limits<int>::max();
        }
        version.value[component++] = number;
    }
    return component > 0;
}

int CompareVersions(const VersionParts& left, const VersionParts& right) {
    for (size_t i = 0; i < ARRAY_SIZE(left.value); ++i)
        if (left.value[i] != right.value[i]) return left.value[i] < right.value[i] ? -1 : 1;
    return 0;
}

bool FetchLatestRelease(ReleaseInfo& release) {
    HINTERNET session = WinHttpOpen(L"PathConfigurator/1.1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    std::string response;
    bool requestOk = false;
    do {
        if (!session) break;
        WinHttpSetTimeouts(session, 1500, 1500, 2500, 2500);
        connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection) break;
        request = WinHttpOpenRequest(connection, L"GET", L"/repos/Name-CK/PathConfigurator/releases/latest",
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) break;
        const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nUser-Agent: PathConfigurator/1.1.0\r\n";
        if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr)) break;
        DWORD statusCode = 0;
        DWORD statusLength = sizeof(statusCode);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusLength, WINHTTP_NO_HEADER_INDEX) || statusCode != 200) break;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) break;
            if (available == 0) { requestOk = true; break; }
            if (response.size() + available > 1024 * 1024) break;
            std::string chunk(available, '\0');
            DWORD received = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &received)) break;
            response.append(chunk.data(), received);
        }
    } while (false);
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
    if (!requestOk) return false;

    std::string tag;
    std::string url;
    if (!ExtractJsonString(response, "tag_name", tag) || !Utf8ToWide(tag, release.tag)) return false;
    if (ExtractJsonString(response, "html_url", url) && !Utf8ToWide(url, release.url)) release.url.clear();
    if (release.url.empty()) release.url = kReleasePage;
    return true;
}

std::wstring CachePath() {
    const std::wstring settings = GetUserDefaultSettingsPath();
    const std::wstring directory = GetParentPath(settings);
    return directory.empty() ? std::wstring{} : JoinPath(directory, L"update-cache.dat");
}

bool WriteCachedRelease(const ReleaseInfo& release) {
    const std::wstring path = CachePath();
    if (path.empty()) return false;
    const std::wstring directory = GetParentPath(path);
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    const std::wstring document = L"PathConfiguratorUpdateCache1\n" + release.tag + L"\n" + release.url;
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const DWORD byteCount = static_cast<DWORD>(document.size() * sizeof(wchar_t));
    DWORD written = 0;
    const bool ok = WriteFile(file, document.data(), byteCount, &written, nullptr) && written == byteCount;
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

} // namespace

bool ReadCachedRelease(ReleaseInfo& release) {
    const std::wstring path = CachePath();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 ||
        (size.QuadPart % sizeof(wchar_t)) != 0) {
        CloseHandle(file);
        return false;
    }
    std::wstring document(static_cast<size_t>(size.QuadPart / sizeof(wchar_t)), L'\0');
    DWORD read = 0;
    const bool ok = ReadFile(file, document.data(), static_cast<DWORD>(size.QuadPart), &read, nullptr) &&
        read == static_cast<DWORD>(size.QuadPart);
    CloseHandle(file);
    if (!ok) return false;
    const std::wstring marker = L"PathConfiguratorUpdateCache1\n";
    if (document.rfind(marker, 0) != 0) return false;
    const size_t tagEnd = document.find(L'\n', marker.size());
    if (tagEnd == std::wstring::npos) return false;
    release.tag = document.substr(marker.size(), tagEnd - marker.size());
    release.url = document.substr(tagEnd + 1);
    return !release.tag.empty() && !release.url.empty();
}

bool IsNewerThan(const std::wstring& currentVersion, const std::wstring& candidateVersion) {
    VersionParts current{};
    VersionParts candidate{};
    return ParseVersion(currentVersion, current) && ParseVersion(candidateVersion, candidate) &&
        CompareVersions(candidate, current) > 0;
}

std::wstring DisplayVersionTag(std::wstring tag) {
    if (!tag.empty() && tag.front() != L'v' && tag.front() != L'V') tag.insert(tag.begin(), L'V');
    return tag;
}

int RefreshCache() {
    ReleaseInfo latest;
    return FetchLatestRelease(latest) && WriteCachedRelease(latest) ? 0 : 1;
}

} // namespace pathconfig::update
