// Windows utilities for the cswap native core: UTF-8/UTF-16 conversion, file
// I/O (atomic replace-on-write), Claude Code path resolution, DPAPI wrappers.

#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <dpapi.h>

#include <string>

namespace cswap {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

inline std::string narrow(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

// ---- files -------------------------------------------------------------------

inline bool fileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool readFile(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char buf[8192];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return true;
}

inline bool writeFileRaw(const std::wstring& path, const void* data, size_t size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = WriteFile(f, data, (DWORD)size, &wrote, nullptr);
    ok = ok && wrote == size && FlushFileBuffers(f);
    CloseHandle(f);
    return ok == TRUE;
}

// Write via a sibling temp file + atomic ReplaceFile/MoveFileEx, so a crash
// mid-write can never leave a truncated credentials/config file behind.
inline bool atomicWriteFile(const std::wstring& path, const std::string& content) {
    std::wstring tmp = path + L".cswap-tmp";
    if (!writeFileRaw(tmp, content.data(), content.size())) return false;
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(tmp.c_str());
    return false;
}

inline bool ensureDir(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// ---- Claude Code paths (mirrors claude_swap.paths) ---------------------------

inline std::wstring envVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return (n && n < MAX_PATH) ? std::wstring(buf) : L"";
}

inline std::wstring homeDir() {
    std::wstring p = envVar(L"USERPROFILE");
    return p.empty() ? L"." : p;
}

// CLAUDE_CONFIG_DIR if set, else ~/.claude
inline std::wstring claudeConfigHome() {
    std::wstring env = envVar(L"CLAUDE_CONFIG_DIR");
    return env.empty() ? homeDir() + L"\\.claude" : env;
}

// <config_home>/.config.json when it exists (legacy), else
// (CLAUDE_CONFIG_DIR || home)/.claude.json — note the asymmetry.
inline std::wstring globalConfigPath() {
    std::wstring legacy = claudeConfigHome() + L"\\.config.json";
    if (fileExists(legacy)) return legacy;
    std::wstring env = envVar(L"CLAUDE_CONFIG_DIR");
    return (env.empty() ? homeDir() : env) + L"\\.claude.json";
}

inline std::wstring credentialsPath() { return claudeConfigHome() + L"\\.credentials.json"; }

// Native store root (separate from the Python fork's .claude-swap-backup).
inline std::wstring vaultDir() { return homeDir() + L"\\.cswap"; }

// ---- DPAPI (current-user scope) ----------------------------------------------

static const wchar_t* DPAPI_DESC = L"cswap account credential";

inline bool dpapiProtect(const std::string& plain, std::string& blobOut) {
    DATA_BLOB in = {(DWORD)plain.size(), (BYTE*)plain.data()};
    DATA_BLOB out = {};
    if (!CryptProtectData(&in, DPAPI_DESC, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    blobOut.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

inline bool dpapiUnprotect(const std::string& blob, std::string& plainOut) {
    DATA_BLOB in = {(DWORD)blob.size(), (BYTE*)blob.data()};
    DATA_BLOB out = {};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plainOut.assign((char*)out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

} // namespace cswap
