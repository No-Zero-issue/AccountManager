// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// SteamClient.cpp —— Steam 一键登录：系统集成
//   会改动 HKCU\\Software\\Valve\\Steam（AutoLoginUser / RememberPassword）
//   并结束正在运行的 steam.exe，因此只在用户点击「登录」并确认后调用。
//   ⚠️ 密码按 Steam 客户端要求以命令行参数传入（-login 用户名 密码），
//      该进程的命令行在登录瞬间可被同机其他进程读取，属已知取舍。

#include "Function.h"

std::atomic<bool> g_steamBusy{ false };

// 读取 Steam 安装路径（注册表 Software\Valve\Steam 的 SteamExe）
std::wstring GetSteamPath() {
    HKEY hKey = NULL;
    WCHAR path[MAX_PATH] = { 0 };
    DWORD size = sizeof(path);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hKey, L"SteamExe", 0, NULL, (BYTE*)path, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::wstring(path);
        }
        RegCloseKey(hKey);
    }
    return L"";
}

// 写入自动登录用户名；必须同时把 RememberPassword 置 1，否则 Steam 会忽略 AutoLoginUser
bool SetAutoLoginUser(const std::wstring& username) {
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS status = RegSetValueExW(hKey, L"AutoLoginUser", 0, REG_SZ,
                                    (const BYTE*)username.c_str(),
                                    (DWORD)(username.length() + 1) * sizeof(wchar_t));
    DWORD remember = 1;
    RegSetValueExW(hKey, L"RememberPassword", 0, REG_DWORD, (const BYTE*)&remember, sizeof(DWORD));

    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

bool IsSteamRunning() {
    bool running = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry;
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0) {
                    running = true;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return running;
}

// 结束所有 steam.exe 进程（Steam 同时只允许一个实例，切换账号必须先关掉）
void KillSteam() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0) {
                HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
                if (process) {
                    TerminateProcess(process, 0);
                    CloseHandle(process);
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

// 带凭据启动 Steam
bool StartSteam(const std::wstring& username, const std::wstring& password, std::wstring& error) {
    error.clear();

    std::wstring steamPath = GetSteamPath();
    if (steamPath.empty()) {
        error = L"SteamExe not found in registry";
        return false;
    }
    for (auto& ch : steamPath) {
        if (ch == L'/') ch = L'\\';
    }

    std::filesystem::path exePath(steamPath);
    std::wstring steamDir = exePath.parent_path().wstring();

    std::wstring args = L"-no-browser -skipinitialbootstrap -nobootstrapupdate -noreactlogin -login " + username;
    if (!password.empty()) {
        args += L" " + password;
    }

    std::wstring cmdLine = L"cmd /c start \"\" \"" + steamPath + L"\" " + args;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    // CreateProcessW 可能需要修改命令行缓冲，必须传入可写且以 0 结尾的缓冲区
    std::vector<wchar_t> buffer(cmdLine.begin(), cmdLine.end());
    buffer.push_back(L'\0');

    BOOL created = CreateProcessW(NULL, buffer.data(), NULL, NULL, FALSE,
                                  CREATE_NO_WINDOW, NULL, steamDir.c_str(), &si, &pi);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (pi.hThread) CloseHandle(pi.hThread);

    if (!created) {
        error = L"CreateProcess failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    return true;
}

// 工作线程：执行一键登录（关闭 Steam -> 写自动登录 -> 启动）
json SteamLoginWorker(std::wstring username, std::wstring password) {
    if (GetSteamPath().empty()) {
        return json{ {"type", "steamLoginResult"}, {"ok", false}, {"code", "steam_not_found"} };
    }

    KillSteam();
    bool registryOk = SetAutoLoginUser(username);

    std::wstring error;
    if (!StartSteam(username, password, error)) {
        return json{
            {"type", "steamLoginResult"},
            {"ok", false},
            {"code", "steam_start_failed"},
            {"detail", WStringToString(error)}
        };
    }

    return json{
        {"type", "steamLoginResult"},
        {"ok", true},
        {"username", WStringToString(username)},
        {"registryOk", registryOk}
    };
}
