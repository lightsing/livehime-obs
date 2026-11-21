#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
struct CloseHandleDeleter {
    void operator()(HANDLE h) const { CloseHandle(h); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
Handle own(HANDLE h) { return Handle(h == INVALID_HANDLE_VALUE ? nullptr : h); }

enum Result { Ok, BadArguments, AlreadyScheduled, HostCheckFailed, VerificationFailed, LaunchFailed };
struct Options {
    DWORD pid = 0;
    fs::path installer, obsDir, bundleDir;
    std::wstring sha256;
};

DWORD parsePid(std::wstring_view text)
{
    unsigned long long value = 0;
    for (const auto c : text) {
        if (c < L'0' || c > L'9') return 0;
        value = value * 10 + (c - L'0');
        if (value > MAXDWORD) return 0;
    }
    return static_cast<DWORD>(value);
}

bool hexDigit(wchar_t c)
{
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f');
}

bool directory(const fs::path &path)
{
    if (!path.is_absolute()) return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool parseOptions(int argc, wchar_t **argv, Options &o)
{
    if (argc != 9 && argc != 11) return false;
    for (int i = 1; i < argc; i += 2) {
        const std::wstring_view key = argv[i];
        const wchar_t *value = argv[i + 1];
        if (!*value) return false;
        if (key == L"--pid" && !o.pid) {
            o.pid = parsePid(value);
            if (!o.pid) return false;
        } else if (key == L"--installer" && o.installer.empty()) o.installer = value;
        else if (key == L"--sha256" && o.sha256.empty()) o.sha256 = value;
        else if (key == L"--obs-dir" && o.obsDir.empty()) o.obsDir = value;
        else if (key == L"--bundle-dir" && o.bundleDir.empty()) o.bundleDir = value;
        else return false;
    }
    return o.pid && o.installer.is_absolute() && _wcsicmp(o.installer.extension().c_str(), L".exe") == 0 &&
           o.sha256.size() == 64 && std::all_of(o.sha256.begin(), o.sha256.end(), hexDigit) &&
           directory(o.obsDir) && (o.bundleDir.empty() || directory(o.bundleDir));
}

bool waitForParent(DWORD pid, const std::wstring &hostName)
{
    auto process = own(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER; // Exited before helper initialization.
    if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) return true;
    std::wstring image(32768, L'\0');
    DWORD size = static_cast<DWORD>(image.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &size))
        return WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
    image.resize(size);
    // On PID reuse the original host is already gone. The system-wide check still follows.
    if (_wcsicmp(fs::path(image).filename().c_str(), hostName.c_str()) != 0) return true;
    return WaitForSingleObject(process.get(), INFINITE) == WAIT_OBJECT_0;
}

enum class Hosts { Idle, Running, Unknown };
Hosts inspectHosts(const std::wstring &hostName)
{
    auto snapshot = own(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!snapshot || !Process32FirstW(snapshot.get(), &entry)) return Hosts::Unknown;
    do {
        if (_wcsicmp(entry.szExeFile, hostName.c_str()) == 0) return Hosts::Running;
    } while (Process32NextW(snapshot.get(), &entry));
    return GetLastError() == ERROR_NO_MORE_FILES ? Hosts::Idle : Hosts::Unknown;
}

bool waitForAllHosts(const std::wstring &hostName)
{
    Hosts state;
    while ((state = inspectHosts(hostName)) == Hosts::Running) Sleep(500);
    return state == Hosts::Idle;
}

bool correctDigest(HANDLE file, const std::wstring &expected)
{
    struct DestroyHash {
        void operator()(BCRYPT_HASH_HANDLE h) const { BCryptDestroyHash(h); }
    };
    BCRYPT_HASH_HANDLE raw = nullptr;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &raw, nullptr, 0, nullptr, 0, 0)))
        return false;
    const std::unique_ptr<void, DestroyHash> hash(raw);
    std::array<UCHAR, 64 * 1024> buffer;
    DWORD bytes;
    while (true) {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytes, nullptr)) return false;
        if (!bytes) break;
        if (!BCRYPT_SUCCESS(BCryptHashData(hash.get(), buffer.data(), bytes, 0))) return false;
    }
    std::array<UCHAR, 32> digest;
    if (!BCRYPT_SUCCESS(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0)))
        return false;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring actual;
    for (auto byte : digest) {
        actual += hex[byte >> 4];
        actual += hex[byte & 15];
    }
    return _wcsicmp(actual.c_str(), expected.c_str()) == 0;
}

fs::path finalPath(HANDLE file)
{
    const DWORD size = GetFinalPathNameByHandleW(file, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!size) return {};
    std::wstring path(size, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(file, path.data(), size, FILE_NAME_NORMALIZED);
    if (!length || length >= size) return {};
    path.resize(length);
    if (path.starts_with(LR"(\\?\UNC\)")) return L"\\\\" + path.substr(8);
    if (path.starts_with(LR"(\\?\)") && path.size() >= 7 && path[5] == L':') return path.substr(4);
    return {};
}

// ShellExecute takes a path, not a file handle. Keep the verified file and its actual
// parent directories open without delete sharing, then check that the pinned name
// still identifies that file. This also resolves junctions in the caller's cache path.
bool pinPath(HANDLE file, const fs::path &path, std::vector<Handle> &directories)
{
    if (path.empty() || _wcsicmp(path.extension().c_str(), L".exe") != 0) return false;
    for (auto dir = path.parent_path(); dir != dir.root_path(); dir = dir.parent_path()) {
        auto handle = own(CreateFileW(dir.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!handle || !GetFileInformationByHandle(handle.get(), &info) ||
            !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return false;
        directories.push_back(std::move(handle));
    }
    auto named = own(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION original{}, current{};
    return named && GetFileInformationByHandle(file, &original) && GetFileInformationByHandle(named.get(), &current) &&
           !(current.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
           original.dwVolumeSerialNumber == current.dwVolumeSerialNumber &&
           original.nFileIndexHigh == current.nFileIndexHigh && original.nFileIndexLow == current.nFileIndexLow;
}

// Windows doubles any trailing backslashes inside a quoted command-line argument.
std::wstring directoryArgument(const wchar_t *key, const fs::path &path)
{
    auto value = path.native();
    const auto last = value.find_last_not_of(L'\\');
    const auto trailing = last == std::wstring::npos ? value.size() : value.size() - last - 1;
    value.append(trailing, L'\\');
    return std::wstring(key) + L"\"" + value + L"\"";
}

Result schedule(const Options &o)
{
    std::wstring hostName = L"obs64.exe", mutexName = L"Local\\LivehimeObsDeferredUpdate";
#ifdef LIVEHIME_UPDATE_TEST_FIXTURE
    wchar_t runId[33]{};
    if (GetEnvironmentVariableW(L"LIVEHIME_UPDATE_FIXTURE_ID", runId, 33) != 32 ||
        !std::all_of(runId, runId + 32, hexDigit)) return BadArguments;
    hostName = std::wstring(L"lh-test-") + runId + L".exe";
    mutexName += std::wstring(L"-fixture-") + runId;
#endif
    SetLastError(ERROR_SUCCESS);
    const HANDLE raw = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    const DWORD mutexError = GetLastError();
    const auto mutex = own(raw);
    if (!mutex) return HostCheckFailed;
    if (mutexError == ERROR_ALREADY_EXISTS) return AlreadyScheduled;
    if (!waitForParent(o.pid, hostName) || !waitForAllHosts(hostName)) return HostCheckFailed;

    auto file = own(CreateFileW(o.installer.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file || GetFileType(file.get()) != FILE_TYPE_DISK) return VerificationFailed;
    const auto installer = finalPath(file.get());
    std::vector<Handle> directories;
    if (!pinPath(file.get(), installer, directories) || !correctDigest(file.get(), o.sha256))
        return VerificationFailed;
    if (!waitForAllHosts(hostName)) return HostCheckFailed; // A new OBS may have started while hashing.

    auto parameters = directoryArgument(L"/OBSDIR=", o.obsDir);
    if (!o.bundleDir.empty()) parameters += L" " + directoryArgument(L"/DIR=", o.bundleDir);
    SHELLEXECUTEINFOW action{sizeof(action)};
    action.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    action.lpVerb = L"open";
    action.lpFile = installer.c_str();
    action.lpParameters = parameters.c_str();
    action.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&action)) return GetLastError() == ERROR_CANCELLED ? Ok : LaunchFailed;
    auto launched = own(action.hProcess);
    if (!launched || WaitForSingleObject(launched.get(), INFINITE) != WAIT_OBJECT_0) return LaunchFailed;
    return Ok;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    auto *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return BadArguments;
    const std::unique_ptr<void, decltype(&LocalFree)> arguments(argv, LocalFree);
    Options options;
    if (!parseOptions(argc, argv, options)) return BadArguments;
    // Do not hold the OBS working directory open during an in-place update.
    wchar_t systemDirectory[MAX_PATH];
    const auto length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    if (!length || length >= MAX_PATH || !SetCurrentDirectoryW(systemDirectory)) return LaunchFailed;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return LaunchFailed;
    const auto result = schedule(options);
    CoUninitialize();
#ifndef LIVEHIME_UPDATE_TEST_FIXTURE
    if (result == HostCheckFailed || result == VerificationFailed || result == LaunchFailed) {
        const wchar_t *message = result == VerificationFailed ? L"无法验证更新安装包，请在 OBS 中重新检查更新。" :
            (result == HostCheckFailed ? L"无法确认 OBS 已退出，请稍后重试。" : L"无法启动更新安装器，请稍后重试。");
        MessageBoxW(nullptr, message, L"Livehime OBS 更新", MB_OK | MB_ICONWARNING);
    }
#endif
    return result;
}
