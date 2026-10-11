// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// AccountManager.cpp : 定义应用程序的入口点。
//

#include "framework.h"
#include "AccountManager.h"
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <fstream>
#include <sstream>
#include <shellapi.h>
#include <filesystem>
#include <bcrypt.h>
#include <winreg.h>
#include <tlhelp32.h>
#include <nlohmann/json.hpp>
#include <dwmapi.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winhttp.h>
#include <ctime>
// C++/WinRT 在 C++17 下会引用已弃用的 <experimental/coroutine>，这里抑制该错误
#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Storage.Streams.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")

using namespace Microsoft::WRL;
using json = nlohmann::json;


#define MAX_LOADSTRING 100

// 后台工作线程完成后向 UI 线程投递结果的自定义消息
#define WM_WORKER_RESULT (WM_APP + 1)

HINSTANCE hInst;                                // 当前实例
WCHAR szTitle[MAX_LOADSTRING];                  // 标题栏文本

// 无边框窗口的初始尺寸（最小可拖拽尺寸见 WM_GETMINMAXINFO：1100x830）
int w = 1280;
int h = 860;

// UTF-8 <-> UTF-16 转换（界面与 C++ 之间的 JSON 一律使用 UTF-8）
std::string WStringToString(const std::wstring& text) {
    if (text.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    if (size <= 0) return std::string();
    std::string out((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size, NULL, NULL);
    return out;
}

std::wstring StringToWString(const std::string& text) {
    if (text.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0);
    if (size <= 0) return std::wstring();
    std::wstring out((size_t)size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), (int)text.size(), &out[0], size);
    return out;
}

// 遍历 WebView2 的子窗口。
// 说明：本应用的拖拽 / 缩放由前端（windowControl 指令）驱动，不依赖子窗口命中测试，
// 因此这里保持不改变任何子窗口状态，避免影响页面内鼠标交互。
BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    (void)hwnd;
    (void)lParam;
    return TRUE;
}

WCHAR szWindowClass[MAX_LOADSTRING];            // 主窗口类名
bool g_isDarkTheme = true;                      // 主题状态
bool g_isHoveringWindowControls = false;        // 窗口控制按钮悬浮状态
bool g_isTrackingMouse = false;                 // 鼠标追踪状态

// WebView2 变量
wil::com_ptr<ICoreWebView2Controller> webviewController;
wil::com_ptr<ICoreWebView2> webview;

// 此代码模块中包含的函数的前向声明:
ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

// ============================================================================
// Steam 一键登录：系统集成
//   会改动 HKCU\Software\Valve\Steam（AutoLoginUser / RememberPassword）
//   并结束正在运行的 steam.exe，因此只在用户点击「登录」并确认后调用。
//   ⚠️ 密码按 Steam 客户端要求以命令行参数传入（-login 用户名 密码），
//      该进程的命令行在登录瞬间可被同机其他进程读取，属已知取舍。
// ============================================================================
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

// ============================================================================


std::wstring GetConfigFilePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::filesystem::path p(path);
    return (p.parent_path() / L"config.json").wstring();
}

// 读取配置
// 读取配置（来自 AccountManager.json 的 config 节）
json ReadConfigJson() {
    try {
        json ReadDataSection(const std::string& name);   // 定义在下方
        json section = ReadDataSection("config");
        return section.is_object() ? section : json::object();
    }
    catch (...) {
        return json::object();
    }
}

// 保存配置
// 保存配置（写入 AccountManager.json 的 config 节，其它节原样保留）
void SaveConfigJson(const json& j) {
    try {
        bool WriteDataSection(const std::string& name, const json& value);   // 定义在下方
        if (!WriteDataSection("config", j)) {
            throw std::runtime_error("cannot write config section");
        }
    }
    catch (const std::exception& e) {
        std::wstring err = L"Config Save Error: " + StringToWString(e.what());
        MessageBoxW(NULL, err.c_str(), L"Error", MB_OK);
    }
}

// ============================================================================
// 密码保险库 vault.json
//   KDF    : PBKDF2-HMAC-SHA256 (BCryptDeriveKeyPBKDF2, 200000 次迭代)
//   加密   : AES-256-CBC + PKCS7 填充 (BCryptEncrypt / BCryptDecrypt)
//   完整性 : HMAC-SHA256(salt || iv || ciphertext)，先加密后校验
//   结构   : { version, kdf:{algorithm,iterations,salt}, cipher, iv, mac, data }
// 说明：解密用的密钥只在解锁期间驻留内存，锁定/退出时清零。
// ============================================================================
// 单文件数据存储：AccountManager.json（vault / hello / config 三节）
json ReadDataSection(const std::string& name);
bool WriteDataSection(const std::string& name, const json& value);
bool RemoveDataSection(const std::string& name);
bool WriteDataRoot(const json& root);
void MigrateLegacyDataFiles();

namespace {



constexpr ULONGLONG kVaultKdfIterations = 200000;
constexpr size_t kVaultAesKeySize = 32;
constexpr size_t kVaultMacKeySize = 32;
constexpr size_t kVaultSaltSize = 16;
constexpr size_t kVaultIvSize = 16;

std::vector<BYTE> g_vaultAesKey;
std::vector<BYTE> g_vaultMacKey;
std::vector<BYTE> g_vaultSalt;
ULONGLONG g_vaultIterations = kVaultKdfIterations;
std::atomic<bool> g_vaultUnlocked{ false };
// Windows Hello 验证发生在工作线程，会读写上面的密钥，因此需要加锁
std::mutex g_vaultKeyMutex;

void WipeBytes(std::vector<BYTE>& buf) {
    if (!buf.empty()) {
        SecureZeroMemory(buf.data(), buf.size());
        buf.clear();
    }
}

void WipeText(std::string& text) {
    if (!text.empty()) {
        SecureZeroMemory(&text[0], text.size());
        text.clear();
    }
}

// 加锁写入密钥并标记为已解锁
void InstallVaultKeys(const std::vector<BYTE>& aesKey, const std::vector<BYTE>& macKey,
                      const std::vector<BYTE>& salt, ULONGLONG iterations) {
    std::lock_guard<std::mutex> guard(g_vaultKeyMutex);
    WipeBytes(g_vaultAesKey);
    WipeBytes(g_vaultMacKey);
    g_vaultAesKey = aesKey;
    g_vaultMacKey = macKey;
    g_vaultSalt = salt;
    g_vaultIterations = iterations;
    g_vaultUnlocked = true;
}

// 加锁取一份密钥副本
void SnapshotVaultKeys(std::vector<BYTE>& aesKey, std::vector<BYTE>& macKey,
                       std::vector<BYTE>& salt, ULONGLONG& iterations, bool& unlocked) {
    std::lock_guard<std::mutex> guard(g_vaultKeyMutex);
    aesKey = g_vaultAesKey;
    macKey = g_vaultMacKey;
    salt = g_vaultSalt;
    iterations = g_vaultIterations;
    unlocked = g_vaultUnlocked;
}

bool RandomBytes(std::vector<BYTE>& out, size_t count) {
    out.resize(count);
    NTSTATUS status = BCryptGenRandom(NULL, out.data(), (ULONG)count, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return status == 0;
}

const std::string kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const std::vector<BYTE>& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= data.size()) {
        unsigned int n = ((unsigned int)data[i] << 16) | ((unsigned int)data[i + 1] << 8) | (unsigned int)data[i + 2];
        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 6) & 0x3F]);
        out.push_back(kBase64Chars[n & 0x3F]);
        i += 3;
    }
    if (i < data.size()) {
        bool two = (i + 1 < data.size());
        unsigned int n = ((unsigned int)data[i] << 16) | (two ? ((unsigned int)data[i + 1] << 8) : 0);
        out.push_back(kBase64Chars[(n >> 18) & 0x3F]);
        out.push_back(kBase64Chars[(n >> 12) & 0x3F]);
        out.push_back(two ? kBase64Chars[(n >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

std::vector<BYTE> Base64Decode(const std::string& text) {
    std::vector<BYTE> out;
    int buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=') break;
        size_t index = kBase64Chars.find(c);
        if (index == std::string::npos) continue;
        buffer = (buffer << 6) | (int)index;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((BYTE)((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

bool DeriveVaultKeys(const std::string& password, const std::vector<BYTE>& salt, ULONGLONG iterations,
                     std::vector<BYTE>& aesKey, std::vector<BYTE>& macKey) {
    BCRYPT_ALG_HANDLE hPrf = NULL;
    if (BCryptOpenAlgorithmProvider(&hPrf, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) {
        return false;
    }

    std::vector<BYTE> derived(kVaultAesKeySize + kVaultMacKeySize);
    NTSTATUS status = BCryptDeriveKeyPBKDF2(
        hPrf,
        (PUCHAR)password.data(), (ULONG)password.size(),
        (PUCHAR)salt.data(), (ULONG)salt.size(),
        iterations,
        derived.data(), (ULONG)derived.size(),
        0);
    BCryptCloseAlgorithmProvider(hPrf, 0);

    if (status != 0) {
        WipeBytes(derived);
        return false;
    }

    aesKey.assign(derived.begin(), derived.begin() + kVaultAesKeySize);
    macKey.assign(derived.begin() + kVaultAesKeySize, derived.end());
    WipeBytes(derived);
    return true;
}

bool AesCbcEncrypt(const std::vector<BYTE>& key, const std::vector<BYTE>& iv, const std::string& plain, std::vector<BYTE>& cipher) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    bool ok = false;

    do {
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) break;
        if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) break;
        if (BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0) break;

        std::vector<BYTE> ivCopy = iv;
        ULONG needed = 0;
        if (BCryptEncrypt(hKey, (PUCHAR)plain.data(), (ULONG)plain.size(), NULL,
                          ivCopy.data(), (ULONG)ivCopy.size(), NULL, 0, &needed, BCRYPT_BLOCK_PADDING) != 0) break;

        cipher.resize(needed);
        ivCopy = iv;
        ULONG written = 0;
        if (BCryptEncrypt(hKey, (PUCHAR)plain.data(), (ULONG)plain.size(), NULL,
                          ivCopy.data(), (ULONG)ivCopy.size(), cipher.data(), needed, &written, BCRYPT_BLOCK_PADDING) != 0) break;

        cipher.resize(written);
        ok = true;
    } while (false);

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool AesCbcDecrypt(const std::vector<BYTE>& key, const std::vector<BYTE>& iv, const std::vector<BYTE>& cipher, std::vector<BYTE>& plain) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    bool ok = false;

    do {
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0) != 0) break;
        if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) break;
        if (BCryptGenerateSymmetricKey(hAlg, &hKey, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0) break;

        std::vector<BYTE> ivCopy = iv;
        ULONG needed = 0;
        if (BCryptDecrypt(hKey, (PUCHAR)cipher.data(), (ULONG)cipher.size(), NULL,
                          ivCopy.data(), (ULONG)ivCopy.size(), NULL, 0, &needed, BCRYPT_BLOCK_PADDING) != 0) break;

        plain.resize(needed);
        ivCopy = iv;
        ULONG written = 0;
        if (BCryptDecrypt(hKey, (PUCHAR)cipher.data(), (ULONG)cipher.size(), NULL,
                          ivCopy.data(), (ULONG)ivCopy.size(), plain.data(), needed, &written, BCRYPT_BLOCK_PADDING) != 0) break;

        plain.resize(written);
        ok = true;
    } while (false);

    if (hKey) BCryptDestroyKey(hKey);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool ComputeHmacSha256(const std::vector<BYTE>& key, const std::vector<BYTE>& data, std::vector<BYTE>& mac) {
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_HASH_HANDLE hHash = NULL;
    bool ok = false;

    do {
        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) break;
        if (BCryptCreateHash(hAlg, &hHash, NULL, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0) break;
        if (!data.empty() && BCryptHashData(hHash, (PUCHAR)data.data(), (ULONG)data.size(), 0) != 0) break;

        mac.resize(32);
        if (BCryptFinishHash(hHash, mac.data(), (ULONG)mac.size(), 0) != 0) break;
        ok = true;
    } while (false);

    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    return ok;
}

bool ConstantTimeEquals(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    BYTE diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= (BYTE)(a[i] ^ b[i]);
    }
    return diff == 0;
}

std::wstring GetVaultFilePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::filesystem::path p(path);
    return (p.parent_path() / L"vault.json").wstring();
}

std::wstring GetLegacyAccountsFilePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::filesystem::path p(path);
    return (p.parent_path() / L"accounts.json").wstring();
}

// 把 payload 加密为保险库 JSON（每次保存都换新的 IV）
bool BuildVaultJson(const std::vector<BYTE>& aesKey, const std::vector<BYTE>& macKey, const std::vector<BYTE>& salt,
                    ULONGLONG iterations, const json& payload, json& outFile) {
    std::vector<BYTE> iv;
    if (!RandomBytes(iv, kVaultIvSize)) return false;

    std::string plain = payload.dump();
    std::vector<BYTE> cipher;
    bool encrypted = AesCbcEncrypt(aesKey, iv, plain, cipher);
    WipeText(plain);
    if (!encrypted) return false;

    std::vector<BYTE> macInput;
    macInput.reserve(salt.size() + iv.size() + cipher.size());
    macInput.insert(macInput.end(), salt.begin(), salt.end());
    macInput.insert(macInput.end(), iv.begin(), iv.end());
    macInput.insert(macInput.end(), cipher.begin(), cipher.end());

    std::vector<BYTE> mac;
    if (!ComputeHmacSha256(macKey, macInput, mac)) return false;

    outFile = json::object();
    outFile["version"] = 1;
    outFile["kdf"] = {
        {"algorithm", "PBKDF2-HMAC-SHA256"},
        {"iterations", iterations},
        {"salt", Base64Encode(salt)}
    };
    outFile["cipher"] = "AES-256-CBC";
    outFile["iv"] = Base64Encode(iv);
    outFile["mac"] = Base64Encode(mac);
    outFile["data"] = Base64Encode(cipher);
    return true;
}


bool WriteVaultFile(const json& fileData, std::wstring& error) {
    try {
        // 写入 AccountManager.json 的 vault 节（读-改-写，不动其它节）
        if (!WriteDataSection("vault", fileData)) {
            error = L"cannot write AccountManager.json vault section";
            return false;
        }
        return true;
    }
    catch (const std::exception& e) {
        error = L"write error: " + StringToWString(e.what());
        return false;
    }
}

// 读取 vault.json 信封（盐 / IV / MAC / 密文）
bool ReadVaultEnvelope(json& fileData, std::vector<BYTE>& salt, std::vector<BYTE>& iv,
                       std::vector<BYTE>& mac, std::vector<BYTE>& cipher, ULONGLONG& iterations,
                       std::string& errorCode) {
    errorCode.clear();

    try {
        fileData = ReadDataSection("vault");
        if (fileData.is_null()) {
            errorCode = "vault_missing";
            return false;
        }
    }
    catch (...) {
        errorCode = "vault_corrupted";
        return false;
    }

    if (!fileData.is_object() || !fileData.contains("kdf") || !fileData.contains("iv")
        || !fileData.contains("mac") || !fileData.contains("data")) {
        errorCode = "vault_corrupted";
        return false;
    }

    std::string algorithm = fileData["kdf"].value("algorithm", "");
    iterations = fileData["kdf"].value("iterations", kVaultKdfIterations);
    salt = Base64Decode(fileData["kdf"].value("salt", ""));
    iv = Base64Decode(fileData.value("iv", ""));
    mac = Base64Decode(fileData.value("mac", ""));
    cipher = Base64Decode(fileData.value("data", ""));

    if (algorithm != "PBKDF2-HMAC-SHA256" || salt.empty() || iv.size() != kVaultIvSize
        || mac.size() != 32 || cipher.empty()) {
        errorCode = "vault_corrupted";
        return false;
    }
    return true;
}

bool DecryptEnvelopeWithKeys(const std::vector<BYTE>& aesKey, const std::vector<BYTE>& macKey,
                             const std::vector<BYTE>& salt, const std::vector<BYTE>& iv,
                             const std::vector<BYTE>& mac, const std::vector<BYTE>& cipher,
                             const char* macErrorCode, json& payload, std::string& errorCode) {
    errorCode.clear();

    std::vector<BYTE> macInput;
    macInput.reserve(salt.size() + iv.size() + cipher.size());
    macInput.insert(macInput.end(), salt.begin(), salt.end());
    macInput.insert(macInput.end(), iv.begin(), iv.end());
    macInput.insert(macInput.end(), cipher.begin(), cipher.end());

    std::vector<BYTE> expected;
    if (!ComputeHmacSha256(macKey, macInput, expected) || !ConstantTimeEquals(expected, mac)) {
        errorCode = macErrorCode;
        return false;
    }

    std::vector<BYTE> plain;
    if (!AesCbcDecrypt(aesKey, iv, cipher, plain)) {
        errorCode = "decrypt_failed";
        return false;
    }

    std::string jsonText((const char*)plain.data(), plain.size());
    WipeBytes(plain);

    bool parsed = true;
    try {
        payload = json::parse(jsonText);
    }
    catch (...) {
        parsed = false;
    }
    WipeText(jsonText);

    if (!parsed) {
        errorCode = "vault_corrupted";
        return false;
    }
    return true;
}

// 主密码路径：读取信封 -> 派生密钥 -> 校验解密 -> 记住密钥并进入解锁状态
bool DecryptVaultFile(const std::string& password, json& payload, std::string& errorCode) {
    json fileData;
    std::vector<BYTE> salt;
    std::vector<BYTE> iv;
    std::vector<BYTE> mac;
    std::vector<BYTE> cipher;
    ULONGLONG iterations = kVaultKdfIterations;
    if (!ReadVaultEnvelope(fileData, salt, iv, mac, cipher, iterations, errorCode)) {
        return false;
    }

    std::vector<BYTE> aesKey;
    std::vector<BYTE> macKey;
    if (!DeriveVaultKeys(password, salt, iterations, aesKey, macKey)) {
        errorCode = "derive_failed";
        return false;
    }

    if (!DecryptEnvelopeWithKeys(aesKey, macKey, salt, iv, mac, cipher, "wrong_password", payload, errorCode)) {
        WipeBytes(aesKey);
        WipeBytes(macKey);
        return false;
    }

    InstallVaultKeys(aesKey, macKey, salt, iterations);
    return true;
}

// Windows Hello 路径：用内存中已解出的保险库密钥解密 vault.json
bool DecryptVaultWithStoredKeys(json& payload, std::string& errorCode) {
    std::vector<BYTE> aesKey;
    std::vector<BYTE> macKey;
    std::vector<BYTE> storedSalt;
    ULONGLONG storedIterations = kVaultKdfIterations;
    bool unlocked = false;
    SnapshotVaultKeys(aesKey, macKey, storedSalt, storedIterations, unlocked);
    if (!unlocked || aesKey.empty() || macKey.empty()) {
        errorCode = "locked";
        return false;
    }

    json fileData;
    std::vector<BYTE> salt;
    std::vector<BYTE> iv;
    std::vector<BYTE> mac;
    std::vector<BYTE> cipher;
    ULONGLONG iterations = storedIterations;
    if (!ReadVaultEnvelope(fileData, salt, iv, mac, cipher, iterations, errorCode)) {
        WipeBytes(aesKey);
        WipeBytes(macKey);
        return false;
    }

    bool ok = DecryptEnvelopeWithKeys(aesKey, macKey, salt, iv, mac, cipher,
                                      "hello_key_mismatch", payload, errorCode);
    WipeBytes(aesKey);
    WipeBytes(macKey);
    return ok;
}

} // namespace

bool VaultFileExists() {
    std::error_code ec;
    std::filesystem::path path = GetVaultFilePath();
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) return true;
    return ReadDataSection("hello").is_object();   // 也接受已合并进 AccountManager.json 的 hello 节
}

// 把旧的 Steam 账号文件（accounts.json）转换为通用条目，便于首次创建保险库时导入
json MigrateLegacyAccounts() {
    json result = json::array();

    std::error_code ec;
    std::filesystem::path path = GetLegacyAccountsFilePath();
    if (!std::filesystem::exists(path, ec) || std::filesystem::file_size(path, ec) == 0) {
        return result;
    }

    try {
        std::ifstream file(path.wstring(), std::ios::binary);
        if (!file.is_open()) return result;

        json j;
        file >> j;
        file.close();

        std::vector<json> items;
        if (j.is_object()) {
            const char* keys[] = { "main", "perfect", "5e", "normal" };
            for (const char* key : keys) {
                if (j.contains(key) && j[key].is_array()) {
                    for (const auto& item : j[key]) items.push_back(item);
                }
            }
        }
        else if (j.is_array()) {
            for (const auto& item : j) items.push_back(item);
        }

        int index = 0;
        for (const auto& item : items) {
            if (!item.is_object()) continue;

            std::string username = item.value("username", "");
            std::string alias = item.value("alias", "");
            std::string steamId = item.value("steamId", "");
            std::string legacyStatus = item.value("status", "");

            // 旧账号本来就是 Steam 账号：直接转成 Steam 类型条目。
            // 状态字段已从功能中移除，原来的状态值保留到备注里，不丢信息。
            std::string notes = "由旧版 accounts.json 导入";
            if (!legacyStatus.empty()) {
                notes += " · 原状态：" + legacyStatus;
            }

            json entry = json::object();
            entry["id"] = "imported_" + std::to_string(++index);
            entry["name"] = alias.empty() ? username : alias;
            entry["type"] = "steam";
            entry["url"] = "";
            entry["path"] = "";
            entry["username"] = username;
            entry["password"] = item.value("password", "");
            entry["steamId"] = steamId;
            entry["totpSecret"] = "";
            entry["recoveryCodes"] = "";
            entry["usedRecoveryCodes"] = json::array();
            entry["category"] = "Steam";
            entry["notes"] = notes;
            result.push_back(entry);
        }
    }
    catch (...) {
        return json::array();
    }

    return result;
}

// 首次创建保险库：用主密码派生密钥并写入空保险库（可带初始条目与分类）
bool VaultCreate(const std::string& password, const json& entries, const json& categories, std::wstring& error) {
    std::vector<BYTE> salt;
    std::vector<BYTE> aesKey;
    std::vector<BYTE> macKey;
    if (!RandomBytes(salt, kVaultSaltSize) || !DeriveVaultKeys(password, salt, kVaultKdfIterations, aesKey, macKey)) {
        error = L"key derivation failed";
        return false;
    }

    json payload = json::object();
    payload["version"] = 1;
    payload["entries"] = entries;
    payload["categories"] = categories;

    json fileData;
    if (!BuildVaultJson(aesKey, macKey, salt, kVaultKdfIterations, payload, fileData) || !WriteVaultFile(fileData, error)) {
        WipeBytes(aesKey);
        WipeBytes(macKey);
        WipeBytes(salt);
        return false;
    }

    InstallVaultKeys(aesKey, macKey, salt, kVaultKdfIterations);
    return true;
}

// 用主密码解锁（同时取出分类列表；旧版 vault.json 没有 categories 字段时返回空数组）
bool VaultUnlock(const std::string& password, json& entries, json& categories, std::string& errorCode) {
    json payload;
    if (!DecryptVaultFile(password, payload, errorCode)) {
        return false;
    }
    entries = payload.value("entries", json::array());
    categories = payload.value("categories", json::array());
    return true;
}

// 保存条目与分类列表（仅在解锁状态下可用，复用内存中的密钥）
bool VaultSave(const json& entries, const json& categories, std::wstring& error) {
    std::vector<BYTE> aesKey;
    std::vector<BYTE> macKey;
    std::vector<BYTE> salt;
    ULONGLONG iterations = kVaultKdfIterations;
    bool unlocked = false;
    SnapshotVaultKeys(aesKey, macKey, salt, iterations, unlocked);
    if (!unlocked || aesKey.empty() || macKey.empty() || salt.empty()) {
        error = L"vault is locked";
        return false;
    }

    json payload = json::object();
    payload["version"] = 1;
    payload["entries"] = entries;
    payload["categories"] = categories;

    json fileData;
    bool built = BuildVaultJson(aesKey, macKey, salt, iterations, payload, fileData);
    WipeBytes(aesKey);
    WipeBytes(macKey);
    if (!built) {
        error = L"encrypt failed";
        return false;
    }
    return WriteVaultFile(fileData, error);
}

// 锁定：清空内存中的密钥
void VaultLock() {
    std::lock_guard<std::mutex> guard(g_vaultKeyMutex);
    WipeBytes(g_vaultAesKey);
    WipeBytes(g_vaultMacKey);
    WipeBytes(g_vaultSalt);
    g_vaultUnlocked = false;
}

bool VaultIsUnlocked() {
    return g_vaultUnlocked;
}

// 修改主密码：先校验当前密码，再用新密码 + 新盐重新加密
bool VaultChangeMasterPassword(const std::string& currentPassword, const std::string& newPassword,
                               json& entries, std::string& errorCode) {
    json payload;
    if (!DecryptVaultFile(currentPassword, payload, errorCode)) {
        return false;
    }
    entries = payload.value("entries", json::array());

    std::vector<BYTE> salt;
    std::vector<BYTE> aesKey;
    std::vector<BYTE> macKey;
    if (!RandomBytes(salt, kVaultSaltSize) || !DeriveVaultKeys(newPassword, salt, kVaultKdfIterations, aesKey, macKey)) {
        errorCode = "derive_failed";
        return false;
    }

    json fileData;
    std::wstring error;
    if (!BuildVaultJson(aesKey, macKey, salt, kVaultKdfIterations, payload, fileData) || !WriteVaultFile(fileData, error)) {
        WipeBytes(aesKey);
        WipeBytes(macKey);
        WipeBytes(salt);
        errorCode = "save_failed";
        return false;
    }

    InstallVaultKeys(aesKey, macKey, salt, kVaultKdfIterations);
    return true;
}
// ============================================================================
// Windows Hello 解锁（Windows 安全中心验证：PIN / 指纹 / 人脸）
//   启用：创建或打开 TPM 保护的 Hello 密钥凭据 -> 对固定挑战签名 ->
//         用 HKDF-SHA256(签名) 派生包装密钥 -> 用它加密
//         {AES 密钥, HMAC 密钥, 盐, 迭代次数} 写入 hello.json。
//         私钥由 TPM 保护，必须通过 PIN / 指纹 / 人脸验证才能完成签名。
//   解锁：读取 hello.json -> 签名挑战（弹出 Windows 安全中心验证）->
//         解出保险库密钥 -> 用其解密 vault.json，无需输入主密码。
//   注意：修改主密码会更换保险库密钥，因此需要重新包装一次 Hello 数据，
//         否则 Hello 解锁会失效（此时仍可用主密码解锁）。
// ============================================================================
namespace {

const wchar_t* kHelloCredentialName = L"AccountManagerVaultKey";
const char* kHelloHkdfSalt = "AccountManager-Hello-Wrap-v1";
const size_t kHelloChallengeSize = 32;

// Hello 验证在工作线程中进行，此标志避免并发触发多个系统验证弹窗
std::atomic<bool> g_helloBusy{ false };

std::wstring GetHelloFilePath() {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::filesystem::path p(path);
    return (p.parent_path() / L"hello.json").wstring();
}

// ---------------------------------------------------------------------------
// 单文件数据存储：所有状态集中在一个 AccountManager.json 里
//   { "version": 1, "vault": {...}, "hello": {...}, "config": {...} }
// 写入一律走"读整份 -> 只替换自己那一节 -> 原子写回"，避免互相覆盖。
// ---------------------------------------------------------------------------

}   // —— 以下为文件作用域的分区存储函数 ——
std::wstring GetDataFilePath() {
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return L"";
    std::filesystem::path p(path);
    return (p.parent_path() / L"AccountManager.json").wstring();
}

// 读取整份数据文件；不存在或解析失败时返回空对象
json ReadDataRoot() {
    // 首次运行：新文件不存在而旧三文件存在时，先合并（旧文件保留不删）
    MigrateLegacyDataFiles();
    const std::wstring file = GetDataFilePath();
    if (file.empty()) return json::object();
    std::ifstream in(std::filesystem::path(file), std::ios::binary);
    if (!in) return json::object();
    try {
        std::stringstream ss;
        ss << in.rdbuf();
        json root = json::parse(ss.str());
        return root.is_object() ? root : json::object();
    } catch (...) {
        return json::object();
    }
}

// 原子写入整份数据文件（先写 .tmp 再替换）
bool WriteDataRoot(const json& root) {
    const std::wstring file = GetDataFilePath();
    if (file.empty()) return false;
    const std::filesystem::path target(file);
    const std::filesystem::path tmp(target.wstring() + L".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << root.dump(2);
        out.flush();
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        std::filesystem::remove(target, ec);
        ec.clear();
        std::filesystem::rename(tmp, target, ec);
    }
    return !ec;
}

// 读某一节；不存在返回 null
json ReadDataSection(const std::string& name) {
    json root = ReadDataRoot();
    if (root.is_object() && root.contains(name)) return root[name];
    return json();
}

// 读-改-写：只更新指定那一节，其它节原样保留
bool WriteDataSection(const std::string& name, const json& value) {
    json root = ReadDataRoot();
    if (!root.is_object()) root = json::object();
    root["version"] = 1;
    root[name] = value;
    return WriteDataRoot(root);
}

// 读-改-写：删除某一节（其它节原样保留）
bool RemoveDataSection(const std::string& name) {
    json root = ReadDataRoot();
    if (!root.is_object() || !root.contains(name)) return true;
    root.erase(name);
    return WriteDataRoot(root);
}

// 首次启动迁移：把旧的三个独立文件合并进 AccountManager.json（旧文件保留不删）
void MigrateLegacyDataFiles() {
    const std::wstring dataFile = GetDataFilePath();
    if (dataFile.empty()) return;
    if (std::filesystem::exists(std::filesystem::path(dataFile))) return;   // 已有新文件

    json root = json::object();
    root["version"] = 1;
    bool any = false;
    const std::pair<const char*, std::wstring> items[] = {
        { "vault",  GetVaultFilePath() },
        { "hello",  GetHelloFilePath() },
        { "config", GetConfigFilePath() },
    };
    for (const auto& item : items) {
        if (item.second.empty()) continue;
        const std::filesystem::path legacy(item.second);
        if (!std::filesystem::exists(legacy)) continue;
        std::ifstream in(legacy, std::ios::binary);
        if (!in) continue;
        try {
            std::stringstream ss;
            ss << in.rdbuf();
            root[item.first] = json::parse(ss.str());
            any = true;
        } catch (...) {
            // 旧文件损坏：跳过这一节，不阻断启动
        }
    }
    if (any) WriteDataRoot(root);
}
namespace {   // —— 恢复匿名命名空间 ——


// 删除 Hello 解锁数据（凭据失效或数据损坏时清理，避免留下无法使用的解锁入口）
void RemoveHelloFile() {
    try {
        std::error_code ec;
        std::filesystem::remove(GetHelloFilePath(), ec);
        RemoveDataSection("hello");   // 同步清掉 AccountManager.json 里的 hello 节
    }
    catch (...) {
        // 忽略
    }
}

// HKDF-SHA256：Extract + 一轮 Expand（输出 32 字节）
bool HkdfSha256(const std::vector<BYTE>& ikm, const std::string& salt, const std::string& info,
                std::vector<BYTE>& out) {
    std::vector<BYTE> saltBytes(salt.begin(), salt.end());
    std::vector<BYTE> prk;
    if (!ComputeHmacSha256(saltBytes, ikm, prk)) {
        return false;
    }

    std::vector<BYTE> block = prk;
    block.insert(block.end(), info.begin(), info.end());
    block.push_back(0x01);

    bool ok = ComputeHmacSha256(prk, block, out);
    WipeBytes(prk);
    WipeBytes(block);
    return ok;
}

bool ReadHelloFile(json& out, std::string& errorCode) {
    errorCode.clear();

    try {
        std::filesystem::path path = GetHelloFilePath();
        // 优先使用 AccountManager.json 的 hello 节（合并后的主存储）；不存在时才回退旧文件
        {
            json helloSection = ReadDataSection("hello");
            if (helloSection.is_object()) {
                std::ofstream seed(path.wstring(), std::ios::binary | std::ios::trunc);
                if (seed) { seed << helloSection.dump(); }
            }
        }
        std::ifstream file(path.wstring(), std::ios::binary);
        if (!file.is_open()) {
            errorCode = "hello_not_enabled";
            return false;
        }
        file >> out;
        file.close();
    }
    catch (...) {
        errorCode = "hello_corrupted";
        return false;
    }

    if (!out.is_object() || !out.contains("challenge") || !out.contains("iv")
        || !out.contains("mac") || !out.contains("data")) {
        errorCode = "hello_corrupted";
        return false;
    }
    return true;
}

// 调用 Windows Hello 对挑战签名；会弹出 Windows 安全中心验证
bool HelloSignChallenge(const std::vector<BYTE>& challenge, bool createIfMissing,
                        std::vector<BYTE>& signature, std::string& errorCode) {
    errorCode.clear();

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...) {
        // 该线程已初始化过 COM，忽略
    }

    try {
        using namespace winrt::Windows::Security::Credentials;

        if (!KeyCredentialManager::IsSupportedAsync().get()) {
            errorCode = "hello_unsupported";
            return false;
        }

        auto opened = KeyCredentialManager::OpenAsync(kHelloCredentialName).get();
        KeyCredential credential{ nullptr };
        if (opened.Status() == KeyCredentialStatus::Success) {
            credential = opened.Credential();
        }
        else if (createIfMissing) {
            auto created = KeyCredentialManager::RequestCreateAsync(
                kHelloCredentialName, KeyCredentialCreationOption::ReplaceExisting).get();
            if (created.Status() != KeyCredentialStatus::Success) {
                errorCode = (created.Status() == KeyCredentialStatus::UserCanceled) ? "hello_canceled" : "hello_unavailable";
                return false;
            }
            credential = created.Credential();
        }
        else {
            errorCode = "hello_not_enrolled";
            return false;
        }

        if (!credential) {
            errorCode = "hello_unavailable";
            return false;
        }

        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(challenge);
        auto challengeBuffer = writer.DetachBuffer();

        auto signResult = credential.RequestSignAsync(challengeBuffer).get();
        if (signResult.Status() != KeyCredentialStatus::Success) {
            errorCode = (signResult.Status() == KeyCredentialStatus::UserCanceled) ? "hello_canceled" : "hello_failed";
            return false;
        }

        winrt::Windows::Storage::Streams::DataReader reader =
            winrt::Windows::Storage::Streams::DataReader::FromBuffer(signResult.Result());
        signature.resize(reader.UnconsumedBufferLength());
        if (!signature.empty()) {
            reader.ReadBytes(signature);
        }
        reader.Close();

        if (signature.empty()) {
            errorCode = "hello_failed";
            return false;
        }
        return true;
    }
    catch (...) {
        errorCode = "hello_failed";
        return false;
    }
}

} // namespace

bool HelloFileExists() {
    std::error_code ec;
    std::filesystem::path path = GetHelloFilePath();
    return std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0;
}

bool HelloIsBusy() {
    return g_helloBusy;
}

void HelloSetBusy(bool busy) {
    g_helloBusy = busy;
}

// 用 Hello 签名派生的包装密钥加密密钥包
bool HelloWrapKeyBlob(const std::vector<BYTE>& signature, const std::vector<BYTE>& challenge,
                      const json& keyBlob, json& outFile) {
    std::vector<BYTE> wrappingKey;
    if (!HkdfSha256(signature, kHelloHkdfSalt,
                    std::string((const char*)challenge.data(), challenge.size()), wrappingKey)) {
        return false;
    }

    std::vector<BYTE> iv;
    if (!RandomBytes(iv, kVaultIvSize)) {
        WipeBytes(wrappingKey);
        return false;
    }

    std::string plain = keyBlob.dump();
    std::vector<BYTE> cipher;
    bool encrypted = AesCbcEncrypt(wrappingKey, iv, plain, cipher);
    WipeText(plain);
    if (!encrypted) {
        WipeBytes(wrappingKey);
        return false;
    }

    std::vector<BYTE> macInput;
    macInput.reserve(iv.size() + cipher.size());
    macInput.insert(macInput.end(), iv.begin(), iv.end());
    macInput.insert(macInput.end(), cipher.begin(), cipher.end());

    std::vector<BYTE> mac;
    bool macOk = ComputeHmacSha256(wrappingKey, macInput, mac);
    WipeBytes(wrappingKey);
    if (!macOk) {
        return false;
    }

    outFile = json::object();
    outFile["version"] = 1;
    outFile["credential"] = WStringToString(kHelloCredentialName);
    outFile["challenge"] = Base64Encode(challenge);
    outFile["iv"] = Base64Encode(iv);
    outFile["mac"] = Base64Encode(mac);
    outFile["data"] = Base64Encode(cipher);
    return true;
}

// 用 Hello 签名派生的包装密钥解出密钥包
bool HelloUnwrapKeyBlob(const std::vector<BYTE>& signature, const std::vector<BYTE>& challenge,
                        const json& fileData, json& keyBlob, std::string& errorCode) {
    errorCode.clear();

    std::vector<BYTE> iv = Base64Decode(fileData.value("iv", ""));
    std::vector<BYTE> mac = Base64Decode(fileData.value("mac", ""));
    std::vector<BYTE> cipher = Base64Decode(fileData.value("data", ""));
    if (iv.size() != kVaultIvSize || mac.size() != 32 || cipher.empty()) {
        errorCode = "hello_corrupted";
        return false;
    }

    std::vector<BYTE> wrappingKey;
    if (!HkdfSha256(signature, kHelloHkdfSalt,
                    std::string((const char*)challenge.data(), challenge.size()), wrappingKey)) {
        errorCode = "derive_failed";
        return false;
    }

    std::vector<BYTE> macInput;
    macInput.reserve(iv.size() + cipher.size());
    macInput.insert(macInput.end(), iv.begin(), iv.end());
    macInput.insert(macInput.end(), cipher.begin(), cipher.end());

    std::vector<BYTE> expected;
    if (!ComputeHmacSha256(wrappingKey, macInput, expected) || !ConstantTimeEquals(expected, mac)) {
        WipeBytes(wrappingKey);
        errorCode = "hello_key_mismatch";
        return false;
    }

    std::vector<BYTE> plain;
    if (!AesCbcDecrypt(wrappingKey, iv, cipher, plain)) {
        WipeBytes(wrappingKey);
        errorCode = "decrypt_failed";
        return false;
    }
    WipeBytes(wrappingKey);

    std::string text((const char*)plain.data(), plain.size());
    WipeBytes(plain);

    bool parsed = true;
    try {
        keyBlob = json::parse(text);
    }
    catch (...) {
        parsed = false;
    }
    WipeText(text);

    if (!parsed) {
        errorCode = "hello_corrupted";
        return false;
    }
    return true;
}

// 工作线程：启用 Hello 解锁（创建/打开凭据 -> 签名 -> 包装密钥 -> 写 hello.json）
json HelloEnableWorker(std::vector<BYTE> aesKey, std::vector<BYTE> macKey,
                       std::vector<BYTE> salt, ULONGLONG iterations) {
    std::string code;

    std::vector<BYTE> challenge;
    if (!RandomBytes(challenge, kHelloChallengeSize)) {
        return json{ {"type", "vaultError"}, {"code", "derive_failed"} };
    }

    std::vector<BYTE> signature;
    if (!HelloSignChallenge(challenge, true, signature, code)) {
        return json{ {"type", "vaultError"}, {"code", code} };
    }

    json keyBlob = json::object();
    keyBlob["aes"] = Base64Encode(aesKey);
    keyBlob["mac"] = Base64Encode(macKey);
    keyBlob["salt"] = Base64Encode(salt);
    keyBlob["iterations"] = iterations;

    json fileData;
    bool wrapped = HelloWrapKeyBlob(signature, challenge, keyBlob, fileData);
    WipeBytes(signature);
    WipeBytes(challenge);
    if (!wrapped) {
        return json{ {"type", "vaultError"}, {"code", "encrypt_failed"} };
    }

    try {
        std::filesystem::path path = GetHelloFilePath();
        std::ofstream file(path.wstring(), std::ios::out | std::ios::trunc | std::ios::binary);
        if (!file.is_open()) {
            return json{ {"type", "vaultError"}, {"code", "save_failed"} };
        }
        std::string content = fileData.dump(4);
        file.write(content.c_str(), (std::streamsize)content.length());
        file.close();
        WriteDataSection("hello", fileData);   // 同步到 AccountManager.json
    }
    catch (...) {
        return json{ {"type", "vaultError"}, {"code", "save_failed"} };
    }

    return json{ {"type", "helloEnabled"} };
}

// 在工作线程里安全执行 Hello 流程：任何异常都转成 vaultError 回传。
// 若让异常逃出线程函数，CRT 会调用 std::terminate()，Debug 版就表现为
// “已执行断点指令(__debugbreak()语句或类似调用)”。
template <typename TWorker>
void RunBackgroundWorker(HWND hWnd, TWorker&& work) {
    std::string payloadText;
    try {
        payloadText = work().dump();
    }
    catch (const std::exception& e) {
        payloadText = json{ {"type", "vaultError"}, {"code", "worker_exception"}, {"detail", e.what()} }.dump();
    }
    catch (...) {
        payloadText = json{ {"type", "vaultError"}, {"code", "worker_exception"} }.dump();
    }

    std::string* payload = new std::string(std::move(payloadText));
    if (!PostMessage(hWnd, WM_WORKER_RESULT, 0, (LPARAM)payload)) {
        delete payload;   // 窗口已销毁时避免泄漏
    }
}

// 工作线程：修改主密码后重新包装 Hello 数据（密钥已更换，不重新包装则 Hello 解锁会失效）
json HelloRewrapWorker(std::vector<BYTE> aesKey, std::vector<BYTE> macKey,
                       std::vector<BYTE> salt, ULONGLONG iterations) {
    if (!HelloFileExists()) {
        return json{ {"type", "masterPasswordChanged"} };
    }

    json result = HelloEnableWorker(aesKey, macKey, salt, iterations);
    if (result.value("type", "") == "helloEnabled") {
        return json{ {"type", "masterPasswordChanged"} };
    }

    // 重新包装失败（例如用户取消了 Hello 验证）：删除 hello.json，避免留下无法使用的解锁入口
    try {
        std::error_code ec;
        std::filesystem::remove(GetHelloFilePath(), ec);
        const bool helloCleared = RemoveDataSection(std::string("hello"));   // 同步清掉合并文件里的 hello 节
    }
    catch (...) {
        // 忽略
    }
    return json{ {"type", "masterPasswordChanged"}, {"helloRewrapFailed", true}, {"code", result.value("code", "")} };
}

// 工作线程：用 Hello 解锁（签名 -> 解出密钥 -> 解密 vault.json）
json HelloUnlockWorker() {
    json fileData;
    std::string code;
    if (!ReadHelloFile(fileData, code)) {
        bool stale = (code == "hello_corrupted");
        if (stale) RemoveHelloFile();
        return json{ {"type", "vaultError"}, {"code", code}, {"helloDisabled", stale} };
    }

    std::vector<BYTE> challenge = Base64Decode(fileData.value("challenge", ""));
    if (challenge.size() != kHelloChallengeSize) {
        RemoveHelloFile();
        return json{ {"type", "vaultError"}, {"code", "hello_corrupted"}, {"helloDisabled", true} };
    }

    std::vector<BYTE> signature;
    if (!HelloSignChallenge(challenge, false, signature, code)) {
        WipeBytes(challenge);
        // 凭据已失效（例如重装系统或改过 PIN）：清理掉无法使用的 hello.json
        bool stale = (code == "hello_not_enrolled");
        if (stale) RemoveHelloFile();
        return json{ {"type", "vaultError"}, {"code", code}, {"helloDisabled", stale} };
    }

    json keyBlob;
    bool unwrapped = HelloUnwrapKeyBlob(signature, challenge, fileData, keyBlob, code);
    WipeBytes(signature);
    WipeBytes(challenge);
    if (!unwrapped) {
        bool stale = (code == "hello_key_mismatch" || code == "hello_corrupted");
        if (stale) RemoveHelloFile();
        return json{ {"type", "vaultError"}, {"code", code}, {"helloDisabled", stale} };
    }

    std::vector<BYTE> aesKey = Base64Decode(keyBlob.value("aes", ""));
    std::vector<BYTE> macKey = Base64Decode(keyBlob.value("mac", ""));
    std::vector<BYTE> salt = Base64Decode(keyBlob.value("salt", ""));
    ULONGLONG iterations = keyBlob.value("iterations", kVaultKdfIterations);
    if (aesKey.size() != kVaultAesKeySize || macKey.size() != kVaultMacKeySize || salt.empty()) {
        return json{ {"type", "vaultError"}, {"code", "hello_corrupted"} };
    }

    InstallVaultKeys(aesKey, macKey, salt, iterations);

    json payload;
    if (!DecryptVaultWithStoredKeys(payload, code)) {
        VaultLock();
        return json{ {"type", "vaultError"}, {"code", code} };
    }

    json entries = payload.value("entries", json::array());
    json categories = payload.value("categories", json::array());
    return json{ {"type", "vaultUnlocked"}, {"entries", entries}, {"categories", categories} };
}

// 工作线程：关闭 Hello 解锁（删除 hello.json 与 Hello 凭据）
json HelloDisableWorker() {
    RemoveHelloFile();

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (...) {
        // 已初始化，忽略
    }

    try {
        winrt::Windows::Security::Credentials::KeyCredentialManager::DeleteAsync(kHelloCredentialName).get();
    }
    catch (...) {
        // 凭据可能不存在，忽略
    }

    return json{ {"type", "helloDisabled"} };
}

// ============================================================================
// Steam Web API 传输层（WinHTTP + 极简 protobuf）
//   只实现 interop 所需的最小 protobuf（varint / length-delimited），不引入
//   第三方协议库；字段号来自 Valve 的 ITwoFactorService / IAuthenticationService
//   接口定义。当前用于 QueryTime：公开接口、无需登录，既验证链路也可用于校时。
// ============================================================================
namespace steampb {

void WriteVarint(std::string& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back((char)((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back((char)(value & 0x7F));
}

void WriteUInt64Field(std::string& out, uint32_t field, uint64_t value) {
    WriteVarint(out, ((uint64_t)field << 3) | 0);   // wire type 0 = varint
    WriteVarint(out, value);
}

void WriteBytesField(std::string& out, uint32_t field, const std::string& value) {
    WriteVarint(out, ((uint64_t)field << 3) | 2);   // wire type 2 = length-delimited
    WriteVarint(out, value.size());
    out.append(value);
}

bool ReadVarint(const std::string& data, size_t& pos, uint64_t& value) {
    value = 0;
    int shift = 0;
    while (pos < data.size() && shift <= 63) {
        uint8_t byte = (uint8_t)data[pos++];
        value |= ((uint64_t)(byte & 0x7F)) << shift;
        if ((byte & 0x80) == 0) return true;
        shift += 7;
    }
    return false;
}

// 遍历字段，取出第一个匹配的 varint 或 length-delimited 字段
bool FindField(const std::string& data, uint32_t wantedField, uint32_t wantedWireType,
               std::string& bytesOut, uint64_t& varintOut) {
    size_t pos = 0;
    while (pos < data.size()) {
        uint64_t tag = 0;
        if (!ReadVarint(data, pos, tag)) return false;

        uint32_t field = (uint32_t)(tag >> 3);
        uint32_t wireType = (uint32_t)(tag & 0x07);

        if (wireType == 0) {
            uint64_t value = 0;
            if (!ReadVarint(data, pos, value)) return false;
            if (field == wantedField && wantedWireType == 0) {
                varintOut = value;
                return true;
            }
        }
        else if (wireType == 2) {
            uint64_t length = 0;
            if (!ReadVarint(data, pos, length)) return false;
            if (length > data.size() - pos) return false;
            if (field == wantedField && wantedWireType == 2) {
                bytesOut.assign(data, pos, (size_t)length);
                return true;
            }
            pos += (size_t)length;
        }
        else if (wireType == 5) {
            if (data.size() - pos < 4) return false;
            pos += 4;
        }
        else if (wireType == 1) {
            if (data.size() - pos < 8) return false;
            if (field == wantedField && wantedWireType == 1) {
                uint64_t value = 0;
                for (int i = 0; i < 8; i++) {
                    value |= ((uint64_t)(uint8_t)data[pos + i]) << (8 * i);
                }
                varintOut = value;
                return true;
            }
            pos += 8;
        }
        else {
            return false;   // 不支持的 wire type
        }
    }
    return false;
}

bool FindVarint(const std::string& data, uint32_t field, uint64_t& value) {
    std::string dummy;
    return FindField(data, field, 0, dummy, value);
}

bool FindBytes(const std::string& data, uint32_t field, std::string& value) {
    uint64_t dummy = 0;
    return FindField(data, field, 2, value, dummy);
}

} // namespace steampb

// 极简 URL 编码（用于表单体）
std::string UrlEncodeForm(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char ch : text) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')
            || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back((char)ch);
        }
        else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0x0F]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

struct SteamHttpResult {
    bool ok = false;
    DWORD statusCode = 0;
    std::string body;
    std::wstring error;
};

// HTTPS 请求（GET / POST）
//   注意：Steam 各接口要求的 HTTP 方法不同（例如 GetPasswordRSAPublicKey 必须 GET，
//   方法不对会返回 405「This API must be called with a HTTP GET request」）。
bool SteamHttpsRequest(bool useGet, const std::wstring& host, const std::wstring& path,
                       const std::string& formBody, SteamHttpResult& result) {
    result = SteamHttpResult{};

    HINTERNET session = WinHttpOpen(L"AccountManager/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.error = L"WinHttpOpen 失败";
        return false;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect
        ? WinHttpOpenRequest(connect, useGet ? L"GET" : L"POST", path.c_str(), NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
        : nullptr;

    bool sent = false;
    if (request) {
        const wchar_t* headers = useGet ? WINHTTP_NO_ADDITIONAL_HEADERS
                                        : L"Content-Type: application/x-www-form-urlencoded\r\n";
        sent = WinHttpSendRequest(request, headers, (DWORD)-1L,
                                  useGet ? NULL : (LPVOID)formBody.data(),
                                  useGet ? 0 : (DWORD)formBody.size(),
                                  useGet ? 0 : (DWORD)formBody.size(), 0) != FALSE
            && WinHttpReceiveResponse(request, NULL) != FALSE;
    }

    if (!sent) {
        result.error = L"网络请求失败 (WinHTTP error " + std::to_wstring(GetLastError()) + L")";
    }
    else {
        DWORD code = 0;
        DWORD size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        result.statusCode = code;

        std::string body;
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0) break;
            body.append(chunk.data(), read);
        }
        result.body = std::move(body);
        result.ok = (code == 200);
        if (!result.ok) {
            result.error = L"HTTP " + std::to_wstring(code);
        }
    }

    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result.ok;
}

bool SteamHttpsPost(const std::wstring& host, const std::wstring& path,
                    const std::string& formBody, SteamHttpResult& result) {
    return SteamHttpsRequest(false, host, path, formBody, result);
}

bool SteamHttpsGet(const std::wstring& host, const std::wstring& path, SteamHttpResult& result) {
    return SteamHttpsRequest(true, host, path, std::string(), result);
}

uint64_t UnixTimeNow() {
    return (uint64_t)std::time(nullptr);
}

// 公开接口 ITwoFactorService/QueryTime：验证链路 + 时间校正
bool SteamQueryTime(uint64_t& serverTime, uint64_t& localTime, std::wstring& error) {
    error.clear();
    localTime = UnixTimeNow();

    std::string payload;
    steampb::WriteUInt64Field(payload, 1, localTime);   // CTwoFactor_Time_Request.sender_time

    std::vector<BYTE> raw(payload.begin(), payload.end());
    std::string body = "input_protobuf_encoded=" + UrlEncodeForm(Base64Encode(raw));

    SteamHttpResult result;
    if (!SteamHttpsPost(L"api.steampowered.com", L"/ITwoFactorService/QueryTime/v1/", body, result)) {
        error = result.error.empty() ? L"请求失败" : result.error;
        return false;
    }

    // 统一服务默认返回 protobuf；若返回 JSON 也一并兼容
    if (!result.body.empty() && result.body[0] == '{') {
        try {
            json j = json::parse(result.body);
            if (j.contains("response") && j["response"].contains("server_time")) {
                serverTime = j["response"]["server_time"].get<uint64_t>();
                return true;
            }
        }
        catch (...) {
            // 落到下面的错误处理
        }
        error = L"JSON 响应解析失败";
        return false;
    }

    if (!steampb::FindVarint(result.body, 1, serverTime)) {   // CTwoFactor_Time_Response.server_time
        error = L"protobuf 响应解析失败";
        return false;
    }
    return true;
}


// ============================================================================
// 从本机 Steam 导入账号
//   读取 <Steam>\config\loginusers.vdf（Valve KeyValues 文本格式）：
//   里面记录了本机登录过的账号 —— SteamID64、AccountName、PersonaName 等，
//   但不含密码（Steam 只保存 refresh token），因此导入后密码需手工填写。
// ============================================================================
namespace vdf {

void SkipWhitespace(const std::string& text, size_t& pos) {
    // 跳过 UTF-8 BOM：部分 Steam 文件带 BOM，不处理会导致解析直接失败
    if (pos == 0 && text.size() >= 3
        && (unsigned char)text[0] == 0xEF
        && (unsigned char)text[1] == 0xBB
        && (unsigned char)text[2] == 0xBF) {
        pos = 3;
    }

    while (pos < text.size()) {
        char ch = text[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            pos++;
            continue;
        }
        // 行注释
        if (ch == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            while (pos < text.size() && text[pos] != '\n') pos++;
            continue;
        }
        break;
    }
}

// 读取一个带引号的 token（KeyValues 文本格式里 key/value 都是引号包裹）
bool ReadToken(const std::string& text, size_t& pos, std::string& out) {
    SkipWhitespace(text, pos);
    if (pos >= text.size() || text[pos] != '"') return false;

    pos++;   // 跳过起始引号
    out.clear();
    while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\' && pos + 1 < text.size()) {
            pos++;
            out.push_back(text[pos++]);
            continue;
        }
        out.push_back(text[pos++]);
    }
    if (pos >= text.size()) return false;
    pos++;   // 跳过结束引号
    return true;
}

// 解析 { ... } 内部
bool ParseObjectBody(const std::string& text, size_t& pos, json& out) {
    out = json::object();
    for (;;) {
        SkipWhitespace(text, pos);
        if (pos >= text.size()) return false;
        if (text[pos] == '}') {
            pos++;
            return true;
        }

        std::string key;
        if (!ReadToken(text, pos, key)) return false;

        SkipWhitespace(text, pos);
        if (pos >= text.size()) return false;

        if (text[pos] == '{') {
            pos++;
            json child;
            if (!ParseObjectBody(text, pos, child)) return false;
            out[key] = child;
        }
        else {
            std::string value;
            if (!ReadToken(text, pos, value)) return false;
            out[key] = value;
        }
    }
}

// 解析整份文档（顶层可能是 "key" { ... } 或 "key" "value"）
bool ParseDocument(const std::string& text, json& out) {
    out = json::object();
    size_t pos = 0;
    for (;;) {
        SkipWhitespace(text, pos);
        if (pos >= text.size()) break;

        std::string key;
        if (!ReadToken(text, pos, key)) break;

        SkipWhitespace(text, pos);
        if (pos < text.size() && text[pos] == '{') {
            pos++;
            json child;
            if (!ParseObjectBody(text, pos, child)) return false;
            out[key] = child;
        }
        else {
            std::string value;
            if (!ReadToken(text, pos, value)) break;
            out[key] = value;
        }
    }
    return out.is_object();
}

} // namespace vdf

// 读取本机 Steam 已登录账号列表（只读，不修改任何 Steam 数据）
json ReadSteamLoginUsers(std::wstring& error) {
    error.clear();
    json accounts = json::array();

    std::wstring steamExe = GetSteamPath();
    if (steamExe.empty()) {
        error = L"注册表里找不到 Steam 安装路径（请先运行一次 Steam）";
        return accounts;
    }

    std::filesystem::path vdfPath = std::filesystem::path(steamExe).parent_path() / L"config" / L"loginusers.vdf";
    std::error_code ec;
    if (!std::filesystem::exists(vdfPath, ec) || std::filesystem::file_size(vdfPath, ec) == 0) {
        error = L"找不到 config\\loginusers.vdf";
        return accounts;
    }

    try {
        std::ifstream file(vdfPath.wstring(), std::ios::binary);
        if (!file.is_open()) {
            error = L"无法打开 loginusers.vdf";
            return accounts;
        }
        std::stringstream buffer;
        buffer << file.rdbuf();
        file.close();

        const std::string raw = buffer.str();
        if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
            error = L"loginusers.vdf 是 UTF-16 编码，暂不支持";
            return accounts;
        }

        json doc;
        if (!vdf::ParseDocument(raw, doc)) {
            error = L"loginusers.vdf 解析失败";
            return accounts;
        }

        if (!doc.contains("users") || !doc["users"].is_object()) {
            error = L"loginusers.vdf 结构不符合预期";
            return accounts;
        }

        for (auto& item : doc["users"].items()) {
            const json& info = item.value();
            if (!info.is_object()) continue;

            json account = json::object();
            account["steamId"] = item.key();
            account["accountName"] = info.value("AccountName", "");
            account["personaName"] = info.value("PersonaName", "");
            account["mostRecent"] = info.value("MostRecent", "0") == "1";
            accounts.push_back(account);
        }
    }
    catch (...) {
        error = L"读取 loginusers.vdf 时出错";
        return json::array();
    }

    return accounts;
}


// ============================================================================
// Steam 账号登录 + 应用内添加手机令牌（AddAuthenticator）
//   字段号来源：Valve 的 steammessages_auth.steamclient.proto 与
//   steammessages_twofactor.steamclient.proto（SteamTracking/Protobufs）。
//   流程：
//     1. GetPasswordRSAPublicKey            取 RSA 公钥
//     2. RSA(PKCS#1 v1.5) 加密密码
//     3. BeginAuthSessionViaCredentials     建立登录会话，拿 client_id / request_id
//     4. （需要时）UpdateAuthSessionWithSteamGuardCode 提交验证码
//        注意：这一步不返回 token，必须接着轮询
//     5. PollAuthSessionStatus              轮询拿 access_token / refresh_token
//     6. AddAuthenticator                   新增手机令牌，服务端返回 shared_secret 等
//     7. FinalizeAddAuthenticator           用短信/邮箱激活码完成登记
//   只有第 6、7 步会改动账号；前 5 步只是登录。
// ============================================================================

namespace steampb {

// fixed64（wire type 1）：小端 8 字节，用于 steamid / serial_number 这类字段
void WriteFixed64Field(std::string& out, uint32_t field, uint64_t value) {
    WriteVarint(out, ((uint64_t)field << 3) | 1);
    for (int i = 0; i < 8; i++) {
        out.push_back((char)((value >> (8 * i)) & 0xFF));
    }
}

} // namespace steampb

std::vector<BYTE> HexToBytes(const std::string& hex) {    std::vector<BYTE> bytes;
    bytes.reserve(hex.size() / 2);
    auto value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = value(hex[i]);
        int lo = value(hex[i + 1]);
        if (hi < 0 || lo < 0) break;
        bytes.push_back((BYTE)((hi << 4) | lo));
    }
    return bytes;
}

std::string BytesToHex(const std::vector<BYTE>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (BYTE byte : bytes) {
        out.push_back(digits[(byte >> 4) & 0x0F]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::string BytesToBase64(const std::vector<BYTE>& bytes) {
    return Base64Encode(bytes);
}

std::string Base64ToBytesString(const std::string& base64) {
    std::vector<BYTE> bytes = Base64Decode(base64);
    return std::string(bytes.begin(), bytes.end());
}

// 用 Steam 返回的 RSA 公钥加密密码（PKCS#1 v1.5，与 SDA / steam-session 一致）
bool RsaEncryptPkcs1(const std::string& modulusHex, const std::string& exponentHex,
                     const std::string& plaintext, std::string& cipherBase64, std::wstring& error) {
    cipherBase64.clear();
    error.clear();

    std::vector<BYTE> modulus = HexToBytes(modulusHex);
    std::vector<BYTE> exponent = HexToBytes(exponentHex);
    while (!modulus.empty() && modulus.front() == 0) modulus.erase(modulus.begin());
    while (!exponent.empty() && exponent.front() == 0) exponent.erase(exponent.begin());
    if (modulus.empty() || exponent.empty()) {
        error = L"RSA 公钥格式不正确";
        return false;
    }

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_KEY_HANDLE key = NULL;
    bool ok = false;

    do {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, NULL, 0) != 0) {
            error = L"BCryptOpenAlgorithmProvider(RSA) 失败";
            break;
        }

        BCRYPT_RSAKEY_BLOB header = {};
        header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
        header.BitLength = (ULONG)(modulus.size() * 8);
        header.cbPublicExp = (ULONG)exponent.size();
        header.cbModulus = (ULONG)modulus.size();

        std::vector<BYTE> blob(sizeof(header) + exponent.size() + modulus.size());
        memcpy(blob.data(), &header, sizeof(header));
        memcpy(blob.data() + sizeof(header), exponent.data(), exponent.size());
        memcpy(blob.data() + sizeof(header) + exponent.size(), modulus.data(), modulus.size());

        if (BCryptImportKeyPair(alg, NULL, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0) != 0) {
            error = L"BCryptImportKeyPair 失败（公钥无效）";
            break;
        }

        ULONG needed = 0;
        if (BCryptEncrypt(key, (PUCHAR)plaintext.data(), (ULONG)plaintext.size(), NULL, NULL, 0,
                          NULL, 0, &needed, BCRYPT_PAD_PKCS1) != 0) {
            error = L"BCryptEncrypt 计算长度失败";
            break;
        }

        std::vector<BYTE> cipher(needed);
        ULONG written = 0;
        if (BCryptEncrypt(key, (PUCHAR)plaintext.data(), (ULONG)plaintext.size(), NULL, NULL, 0,
                          cipher.data(), (ULONG)cipher.size(), &written, BCRYPT_PAD_PKCS1) != 0) {
            error = L"BCryptEncrypt 加密失败";
            break;
        }
        cipher.resize(written);
        cipherBase64 = BytesToBase64(cipher);
        ok = true;
    } while (false);

    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// 统一服务调用：POST 表单 input_protobuf_encoded=<base64(protobuf)>
//   routingQuery：部分接口要求某些字段同时作为查询串里的「routing parameter」
//   （例如 UpdateAuthSessionWithSteamGuardCode 缺少 client_id/steamid 会返回
//    400 Missing required routing parameter），这里一并带上。
bool SteamUnifiedCall(const std::wstring& method, const std::string& protoPayload,
                      const std::wstring& accessToken, const std::string& routingQuery,
                      std::string& responseBody, std::wstring& error) {
    responseBody.clear();
    error.clear();

    std::string query = routingQuery;
    if (!accessToken.empty()) {
        if (!query.empty()) query += "&";
        query += "access_token=" + UrlEncodeForm(std::string(accessToken.begin(), accessToken.end()));
    }

    std::wstring path = L"/" + method + L"/v1/";
    if (!query.empty()) {
        path += L"?" + std::wstring(query.begin(), query.end());
    }

    std::vector<BYTE> raw(protoPayload.begin(), protoPayload.end());
    std::string body = "input_protobuf_encoded=" + UrlEncodeForm(Base64Encode(raw));

    SteamHttpResult result;
    if (!SteamHttpsPost(L"api.steampowered.com", path, body, result)) {
        error = result.error.empty() ? L"网络请求失败" : result.error;
        return false;
    }
    responseBody = result.body;
    return true;
}

// 统一服务响应通常是 protobuf；这里兼容 JSON（?format 形式）
bool SteamResponseValue(const std::string& body, const std::string& jsonField,
                        uint32_t protoField, uint32_t wireType,
                        std::string& bytesOut, uint64_t& varintOut) {
    if (!body.empty() && body[0] == '{') {
        try {
            json j = json::parse(body);
            if (!j.contains("response")) return false;
            const json& r = j["response"];
            if (!r.contains(jsonField)) return false;
            if (wireType == 2) {
                bytesOut = r[jsonField].is_string() ? r[jsonField].get<std::string>() : r[jsonField].dump();
                return true;
            }
            varintOut = r[jsonField].get<uint64_t>();
            return true;
        }
        catch (...) {
            return false;
        }
    }
    return steampb::FindField(body, protoField, wireType, bytesOut, varintOut);
}

// 登录 / 令牌登记会话（仅在内存中，不落盘；取消或完成后清零）
struct SteamEnrollSession {
    bool active = false;
    std::string accountName;
    std::string password;         // 仅在本次会话内存中短暂存在
    uint64_t steamId = 0;
    uint64_t clientId = 0;
    std::string requestId;        // bytes
    std::string accessToken;
    std::string refreshToken;
    std::string deviceId;
    bool guardSubmitted = false;
    std::string mode = "add";        // add = 新增令牌，remove = 移除令牌
    int guardType = 0;

    // AddAuthenticator 返回的登记数据（finalize 成功后有效）
    std::string sharedSecret;
    std::string identitySecret;
    std::string revocationCode;
    std::string uri;
    std::string accountNameFromServer;
    int confirmType = 0;
    std::string phoneHint;
    uint64_t serverTime = 0;

    void Clear() {
        if (!password.empty()) SecureZeroMemory(&password[0], password.size());
        password.clear();
        accessToken.clear();
        refreshToken.clear();
        requestId.clear();
        sharedSecret.clear();
        identitySecret.clear();
        revocationCode.clear();
        uri.clear();
        accountName.clear();
        accountNameFromServer.clear();
        phoneHint.clear();
        deviceId.clear();
        steamId = 0;
        clientId = 0;
        guardSubmitted = false;
        guardType = 0;
        confirmType = 0;
        serverTime = 0;
        active = false;
    }
};

SteamEnrollSession g_steamEnroll;
std::mutex g_steamEnrollMutex;

// 生成 android:<guid> 形式的设备标识
std::string MakeSteamDeviceId() {
    std::vector<BYTE> random(16);
    if (BCryptGenRandom(NULL, random.data(), (ULONG)random.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (size_t i = 0; i < random.size(); i++) random[i] = (BYTE)(rand() & 0xFF);
    }
    std::string hex = BytesToHex(random);
    return "android:" + hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4)
        + "-" + hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

// 令牌登记过程日志：写在程序目录 steam_enroll.log（UTF-8），用于排查
// 「卡在某一步」这类无法在开发机上复现的问题。
void SteamEnrollLog(const std::string& text) {
    try {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);

        wchar_t modulePath[MAX_PATH] = {};
        if (GetModuleFileNameW(NULL, modulePath, MAX_PATH) == 0) return;
        std::filesystem::path logPath = std::filesystem::path(modulePath).parent_path() / L"steam_enroll.log";

        std::ofstream out(logPath, std::ios::app | std::ios::binary);
        if (!out.is_open()) return;
        out << "[" << UnixTimeNow() << "] " << text << "\n";
    }
    catch (...) {
        // 日志失败不影响主流程
    }
}

// 第 1 步：取 RSA 公钥
//   该接口只接受 HTTP GET，且入参要走查询串里的 input_protobuf_encoded
//   （用 ?account_name= 这种友好参数会被拒绝），响应是 protobuf。
bool SteamGetPasswordRsaKey(const std::string& accountName, std::string& modulus, std::string& exponent,
                            uint64_t& timestamp, std::wstring& error) {
    std::string payload;
    steampb::WriteBytesField(payload, 1, accountName);   // account_name
    std::vector<BYTE> raw(payload.begin(), payload.end());

    std::string path = "/IAuthenticationService/GetPasswordRSAPublicKey/v1/?input_protobuf_encoded="
        + UrlEncodeForm(Base64Encode(raw));
    std::wstring wpath(path.begin(), path.end());

    SteamHttpResult response;
    if (!SteamHttpsGet(L"api.steampowered.com", wpath, response)) {
        error = response.error.empty() ? L"请求失败" : response.error;
        return false;
    }

    std::string body = response.body;
    std::string mod, exp;
    uint64_t ts = 0;
    if (!SteamResponseValue(body, "publickey_mod", 1, 2, mod, ts)
        || !SteamResponseValue(body, "publickey_exp", 2, 2, exp, ts)
        || !SteamResponseValue(body, "timestamp", 3, 0, mod, ts)) {
        error = L"RSA 公钥响应解析失败";
        return false;
    }
    modulus = mod;
    exponent = exp;
    timestamp = ts;
    return true;
}

// 第 3 步：建立登录会话
bool SteamBeginAuthSession(const std::string& accountName, const std::string& password,
                           uint64_t encryptionTimestamp, std::wstring& error) {
    std::string payload;
    steampb::WriteBytesField(payload, 1, "AccountManager");        // device_friendly_name
    steampb::WriteBytesField(payload, 2, accountName);             // account_name
    steampb::WriteBytesField(payload, 3, password);                // encrypted_password（base64 文本）
    steampb::WriteVarint(payload, ((uint64_t)4 << 3) | 0);
    steampb::WriteVarint(payload, encryptionTimestamp);            // encryption_timestamp
    steampb::WriteVarint(payload, ((uint64_t)5 << 3) | 0);
    steampb::WriteVarint(payload, 1);                              // remember_login = true
    steampb::WriteVarint(payload, ((uint64_t)6 << 3) | 0);
    steampb::WriteVarint(payload, 1);                              // platform_type = SteamClient

    // device_details { device_friendly_name = 1, platform_type = 2 }
    std::string deviceDetails;
    steampb::WriteBytesField(deviceDetails, 1, "AccountManager");
    steampb::WriteVarint(deviceDetails, ((uint64_t)2 << 3) | 0);
    steampb::WriteVarint(deviceDetails, 1);
    steampb::WriteBytesField(payload, 9, deviceDetails);

    std::string body;
    if (!SteamUnifiedCall(L"IAuthenticationService/BeginAuthSessionViaCredentials", payload, L"", std::string(), body, error)) {
        return false;
    }

    std::string requestId;
    uint64_t clientId = 0, steamId = 0;
    if (!SteamResponseValue(body, "client_id", 1, 0, requestId, clientId)) {
        error = L"登录响应缺少 client_id（账号名或密码可能不正确）";
        return false;
    }
    SteamResponseValue(body, "request_id", 2, 2, requestId, clientId);
    SteamResponseValue(body, "steamid", 5, 0, requestId, steamId);

    std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
    g_steamEnroll.clientId = clientId;
    g_steamEnroll.requestId = requestId;
    g_steamEnroll.steamId = steamId;

    // 解析 allowed_confirmations（repeated 消息，逐个取出 confirmation_type 与 associated_message）
    {
        std::string details;
        uint64_t dummy = 0;
        (void)dummy;
        // 直接扫描：找所有 field 4 的 length-delimited 子消息
        size_t pos = 0;
        bool sawDeviceCode = false, sawEmailCode = false, sawDeviceConfirm = false, sawEmailConfirm = false;
        std::string assocMessage;
        while (pos < body.size()) {
            uint64_t tag = 0;
            if (!steampb::ReadVarint(body, pos, tag)) break;
            uint32_t field = (uint32_t)(tag >> 3);
            uint32_t wireType = (uint32_t)(tag & 7);
            if (wireType == 0) {
                uint64_t skip = 0;
                if (!steampb::ReadVarint(body, pos, skip)) break;
            }
            else if (wireType == 2) {
                uint64_t length = 0;
                if (!steampb::ReadVarint(body, pos, length)) break;
                if (length > body.size() - pos) break;
                if (field == 4) {
                    std::string sub = body.substr(pos, (size_t)length);
                    uint64_t confirmType = 0;
                    std::string message;
                    if (steampb::FindVarint(sub, 1, confirmType)) {
                        if (confirmType == 2) sawEmailCode = true;
                        else if (confirmType == 3) sawDeviceCode = true;
                        else if (confirmType == 4) sawDeviceConfirm = true;
                        else if (confirmType == 5) sawEmailConfirm = true;
                    }
                    if (steampb::FindBytes(sub, 2, message)) assocMessage = message;
                }
                pos += (size_t)length;
            }
            else if (wireType == 1) { if (body.size() - pos < 8) break; pos += 8; }
            else if (wireType == 5) { if (body.size() - pos < 4) break; pos += 4; }
            else break;
        }

        // 兼容：如果 Steam 这次回的是 JSON（而非 protobuf），同样解析 allowed_confirmations，
        // 否则会漏掉「需要邮箱验证码」而直接进入无意义的长时间轮询。
        if (!sawDeviceCode && !sawEmailCode && !sawDeviceConfirm && !sawEmailConfirm
            && !body.empty() && body[0] == '{') {
            try {
                json j = json::parse(body);
                if (j.contains("response") && j["response"].contains("allowed_confirmations")) {
                    for (auto& item : j["response"]["allowed_confirmations"]) {
                        int ct = item.value("confirmation_type", 0);
                        if (ct == 2) sawEmailCode = true;
                        else if (ct == 3) sawDeviceCode = true;
                        else if (ct == 4) sawDeviceConfirm = true;
                        else if (ct == 5) sawEmailConfirm = true;
                        std::string msg = item.value("associated_message", "");
                        if (!msg.empty()) assocMessage = msg;
                    }
                }
            }
            catch (...) {
                // 解析失败则维持原判断
            }
        }

        SteamEnrollLog("begin_session ok: client_id=" + std::to_string(clientId)
            + " steamid=" + std::to_string(steamId)
            + " bodyLen=" + std::to_string(body.size())
            + " isJson=" + ((!body.empty() && body[0] == '{') ? "yes" : "no")
            + " emailCode=" + (sawEmailCode ? "1" : "0")
            + " deviceCode=" + (sawDeviceCode ? "1" : "0")
            + " deviceConfirm=" + (sawDeviceConfirm ? "1" : "0")
            + " emailConfirm=" + (sawEmailConfirm ? "1" : "0"));

        // 注意：此处不能再次加锁 —— 函数上文已持有 g_steamEnrollMutex，
        // std::mutex 非递归，重复加锁会自死锁（曾导致向导永久停在「登录中…」）。
        if (sawDeviceCode) g_steamEnroll.guardType = 3;
        else if (sawEmailCode) g_steamEnroll.guardType = 2;
        else if (sawDeviceConfirm) g_steamEnroll.guardType = 4;
        else if (sawEmailConfirm) g_steamEnroll.guardType = 5;
        else g_steamEnroll.guardType = 0;
        g_steamEnroll.phoneHint = assocMessage;
    }
    return true;
}

// 第 4 步：提交 Steam Guard / 邮箱验证码（此调用不返回 token）
bool SteamSubmitGuardCode(int codeType, const std::string& code, std::wstring& error) {
    uint64_t clientId = 0;
    uint64_t steamId = 0;
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        clientId = g_steamEnroll.clientId;
        steamId = g_steamEnroll.steamId;
    }

    std::string payload;
    steampb::WriteVarint(payload, ((uint64_t)1 << 3) | 0);
    steampb::WriteVarint(payload, clientId);           // client_id
    steampb::WriteFixed64Field(payload, 2, steamId);   // steamid (fixed64)
    steampb::WriteBytesField(payload, 3, code);        // code
    steampb::WriteVarint(payload, ((uint64_t)4 << 3) | 0);
    steampb::WriteVarint(payload, (uint64_t)codeType); // code_type

    std::string body;
    std::string routing = "client_id=" + std::to_string(clientId) + "&steamid=" + std::to_string(steamId);
    if (!SteamUnifiedCall(L"IAuthenticationService/UpdateAuthSessionWithSteamGuardCode", payload, L"", routing, body, error)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
    g_steamEnroll.guardSubmitted = true;
    return true;
}

// 第 5 步：轮询登录状态，拿到 access_token / refresh_token
bool SteamPollAuthSession(int maxAttempts, std::wstring& error) {
    for (int attempt = 0; attempt < maxAttempts; attempt++) {
        uint64_t clientId = 0;
        std::string requestId;
        {
            std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
            clientId = g_steamEnroll.clientId;
            requestId = g_steamEnroll.requestId;
        }

        std::string payload;
        steampb::WriteVarint(payload, ((uint64_t)1 << 3) | 0);
        steampb::WriteVarint(payload, clientId);              // client_id
        steampb::WriteBytesField(payload, 2, requestId);      // request_id (bytes)

        std::string body;
        std::wstring callError;
        std::string routing = "client_id=" + std::to_string(clientId);
        if (SteamUnifiedCall(L"IAuthenticationService/PollAuthSessionStatus", payload, L"", routing, body, callError)) {
            std::string refreshToken, accessToken;
            uint64_t dummy = 0;
            if (SteamResponseValue(body, "refresh_token", 3, 2, refreshToken, dummy)
                && SteamResponseValue(body, "access_token", 4, 2, accessToken, dummy)
                && !accessToken.empty()) {
                std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
                g_steamEnroll.refreshToken = refreshToken;
                g_steamEnroll.accessToken = accessToken;
                return true;
            }
        }
        else {
            error = callError;
            return false;
        }

        SteamEnrollLog("poll attempt " + std::to_string(attempt + 1) + "/" + std::to_string(maxAttempts)
            + ": 尚未取得 token（登录未完成或仍等待验证）");

        if (attempt + 1 < maxAttempts) {
            Sleep(3000);
        }
    }

    error = L"轮询登录状态超时，请重试";
    return false;
}

// 第 6 步：新增手机令牌（这一步会真正改动账号）
bool SteamAddAuthenticator(std::wstring& error) {
    uint64_t steamId = 0;
    std::string accessToken;
    std::string deviceId;
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        steamId = g_steamEnroll.steamId;
        accessToken = g_steamEnroll.accessToken;
        if (g_steamEnroll.deviceId.empty()) g_steamEnroll.deviceId = MakeSteamDeviceId();
        deviceId = g_steamEnroll.deviceId;
    }
    if (accessToken.empty()) {
        error = L"尚未登录，无法添加令牌";
        return false;
    }

    std::string payload;
    steampb::WriteFixed64Field(payload, 1, steamId);           // steamid (fixed64)
    steampb::WriteVarint(payload, ((uint64_t)2 << 3) | 0);
    steampb::WriteVarint(payload, UnixTimeNow());              // authenticator_time
    steampb::WriteFixed64Field(payload, 3, 1);                 // serial_number（占位，服务端返回真实值）
    steampb::WriteVarint(payload, ((uint64_t)4 << 3) | 0);
    steampb::WriteVarint(payload, 1);                          // authenticator_type = 1
    steampb::WriteBytesField(payload, 5, deviceId);            // device_identifier
    steampb::WriteVarint(payload, ((uint64_t)8 << 3) | 0);
    steampb::WriteVarint(payload, 1);                          // version = 1

    std::string body;
    std::wstring callError;
    std::wstring wideToken(accessToken.begin(), accessToken.end());
    std::string routing = "steamid=" + std::to_string(steamId);
    if (!SteamUnifiedCall(L"ITwoFactorService/AddAuthenticator", payload, wideToken, routing, body, callError)) {
        error = callError;
        return false;
    }

    std::string sharedSecret, identitySecret, revocationCode, uri, accountName, phoneHint;
    uint64_t confirmType = 0, serverTime = 0;
    std::string dummy;

    if (!SteamResponseValue(body, "shared_secret", 1, 2, sharedSecret, confirmType)) {
        // 常见失败原因：账号已存在令牌 / 无法添加
        error = L"添加令牌失败（账号可能已存在令牌，或需要先在手机上移除）";
        return false;
    }
    SteamResponseValue(body, "revocation_code", 3, 2, revocationCode, confirmType);
    SteamResponseValue(body, "uri", 4, 2, uri, confirmType);
    SteamResponseValue(body, "server_time", 5, 0, dummy, serverTime);
    SteamResponseValue(body, "account_name", 6, 2, accountName, confirmType);
    SteamResponseValue(body, "identity_secret", 8, 2, identitySecret, confirmType);
    SteamResponseValue(body, "confirm_type", 12, 0, dummy, confirmType);
    SteamResponseValue(body, "phone_number_hint", 11, 2, phoneHint, confirmType);

    std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
    // 响应里的 shared_secret / identity_secret 是 protobuf 的原始字节，
    // 这里统一「编码」成 base64 文本存储（前端与 SteamGuardCodeForTime 都按 base64 处理）。
    g_steamEnroll.sharedSecret = BytesToBase64(std::vector<BYTE>(sharedSecret.begin(), sharedSecret.end()));
    g_steamEnroll.identitySecret = BytesToBase64(std::vector<BYTE>(identitySecret.begin(), identitySecret.end()));
    g_steamEnroll.revocationCode = revocationCode;
    g_steamEnroll.uri = uri;
    g_steamEnroll.accountNameFromServer = accountName;
    g_steamEnroll.confirmType = (int)confirmType;
    g_steamEnroll.phoneHint = phoneHint;
    g_steamEnroll.serverTime = serverTime;
    return true;
}

// 计算 Steam Guard 5 位码（HMAC-SHA1 + 26 字母表），
// 用于 FinalizeAddAuthenticator 的 authenticator_code 字段。
std::string SteamGuardCodeForTime(const std::string& sharedSecretBase64, uint64_t unixSeconds) {
    std::vector<BYTE> key = Base64Decode(sharedSecretBase64);
    if (key.empty()) return std::string();

    uint64_t counter = unixSeconds / 30;
    BYTE message[8] = {};
    for (int i = 0; i < 8; i++) message[7 - i] = (BYTE)((counter >> (8 * i)) & 0xFF);

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    std::string code;

    do {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) break;
        if (BCryptCreateHash(alg, &hash, NULL, 0, key.data(), (ULONG)key.size(), 0) != 0) break;
        if (BCryptHashData(hash, message, (ULONG)sizeof(message), 0) != 0) break;
        BYTE mac[20] = {};
        if (BCryptFinishHash(hash, mac, (ULONG)sizeof(mac), 0) != 0) break;

        int offset = mac[19] & 0x0F;
        uint32_t value = ((uint32_t)(mac[offset] & 0x7F) << 24)
            | ((uint32_t)mac[offset + 1] << 16)
            | ((uint32_t)mac[offset + 2] << 8)
            | (uint32_t)mac[offset + 3];

        static const char* alphabet = "23456789BCDFGHJKMNPQRTVWXY";
        for (int i = 0; i < 5; i++) {
            code += alphabet[value % 26];
            value /= 26;
        }
    } while (false);

    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return code;
}

// 移除手机令牌（用撤销码）。该接口没有 steamid 字段，但需要已登录的 access_token，
// 且 revocation_code 必须与账号当前令牌匹配；Steam 会通过 revocation_attempts_remaining 限制重试次数。
bool SteamRemoveAuthenticator(const std::string& revocationCode, std::wstring& error) {
    uint64_t steamId = 0;
    std::string accessToken;
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        steamId = g_steamEnroll.steamId;
        accessToken = g_steamEnroll.accessToken;
    }
    if (accessToken.empty()) {
        error = L"尚未登录，无法移除令牌";
        return false;
    }
    if (revocationCode.empty()) {
        error = L"请填写撤销码";
        return false;
    }

    std::string payload;
    steampb::WriteBytesField(payload, 2, revocationCode);   // revocation_code

    std::string body;
    std::wstring callError;
    std::wstring wideToken(accessToken.begin(), accessToken.end());
    std::string routing = "steamid=" + std::to_string(steamId);
    if (!SteamUnifiedCall(L"ITwoFactorService/RemoveAuthenticator", payload, wideToken, routing, body, callError)) {
        error = callError;
        return false;
    }

    uint64_t success = 0;
    uint64_t attemptsLeft = 0;
    std::string dummy;
    SteamResponseValue(body, "success", 1, 0, dummy, success);
    SteamResponseValue(body, "revocation_attempts_remaining", 5, 0, dummy, attemptsLeft);
    SteamEnrollLog("remove response: success=" + std::to_string(success)
        + " attemptsLeft=" + std::to_string(attemptsLeft)
        + " bodyLen=" + std::to_string(body.size()));
    if (success == 0) {
        error = L"撤销码不正确（Steam 剩余尝试次数：" + std::to_wstring(attemptsLeft) + L"）";
        return false;
    }

    std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
    g_steamEnroll.sharedSecret.clear();
    g_steamEnroll.identitySecret.clear();
    g_steamEnroll.revocationCode.clear();
    return true;
}

// 第 7 步：用短信/邮箱激活码完成登记
bool SteamFinalizeAddAuthenticator(const std::string& activationCode, std::wstring& error) {
    uint64_t steamId = 0;
    uint64_t serverTime = 0;
    std::string accessToken;
    std::string sharedSecret;
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        steamId = g_steamEnroll.steamId;
        serverTime = g_steamEnroll.serverTime;
        accessToken = g_steamEnroll.accessToken;
        sharedSecret = g_steamEnroll.sharedSecret;
    }
    if (accessToken.empty()) {
        error = L"尚未登录，无法完成令牌登记";
        return false;
    }

    // FinalizeAddAuthenticator 除了激活码，还必须带上「用新密钥算出的当前动态码」，
    // 以及 AddAuthenticator 返回的 server_time；缺任一项 Steam 都会直接返回失败。
    uint64_t authenticatorTime = serverTime != 0 ? serverTime : UnixTimeNow();
    std::string guardCode = SteamGuardCodeForTime(sharedSecret, authenticatorTime);
    if (guardCode.empty()) {
        error = L"无法生成令牌动态码（会话里缺少密钥）";
        return false;
    }
    SteamEnrollLog("finalize: time=" + std::to_string(authenticatorTime)
        + " guardCodeLen=" + std::to_string(guardCode.size())
        + " secretLen=" + std::to_string(sharedSecret.size()));

    std::string payload;
    steampb::WriteFixed64Field(payload, 1, steamId);         // steamid (fixed64)
    steampb::WriteBytesField(payload, 2, guardCode);         // authenticator_code（之前漏了此字段）
    steampb::WriteVarint(payload, ((uint64_t)3 << 3) | 0);
    steampb::WriteVarint(payload, authenticatorTime);        // authenticator_time
    steampb::WriteBytesField(payload, 4, activationCode);    // activation_code

    std::string body;
    std::wstring callError;
    std::wstring wideToken(accessToken.begin(), accessToken.end());
    std::string routing = "steamid=" + std::to_string(steamId);
    if (!SteamUnifiedCall(L"ITwoFactorService/FinalizeAddAuthenticator", payload, wideToken, routing, body, callError)) {
        error = callError;
        return false;
    }

    uint64_t success = 0;
    uint64_t status = 0;
    std::string dummy;
    SteamResponseValue(body, "success", 1, 0, dummy, success);
    SteamResponseValue(body, "status", 4, 0, dummy, status);
    SteamEnrollLog("finalize response: success=" + std::to_string(success)
        + " status=" + std::to_string(status) + " bodyLen=" + std::to_string(body.size()));
    if (success == 0) {
        error = L"激活失败（Steam status=" + std::to_wstring(status)
            + L"）：请确认填的是最新一条验证码，或稍后重试";
        return false;
    }
    return true;
}

std::atomic<bool> g_steamEnrollBusy(false);

json SteamEnrollErrorJson(const std::string& code, const std::wstring& detail) {
    SteamEnrollLog("ERROR code=" + code + " detail=" + WStringToString(detail));
    return json{
        {"type", "steamEnrollError"},
        {"code", code},
        {"detail", WStringToString(detail)}
    };
}

// 无论正常返回还是抛异常，都释放 busy 标记
struct SteamEnrollBusyGuard {
    ~SteamEnrollBusyGuard() { g_steamEnrollBusy = false; }
};

json SteamEnrollStepJson(const std::string& step, int codeType, const std::string& message) {
    SteamEnrollLog("STEP " + step + " codeType=" + std::to_string(codeType) + " message=" + message);
    return json{
        {"type", "steamEnrollStep"},
        {"step", step},
        {"codeType", codeType},
        {"message", message}
    };
}

// 登录成功后：新增令牌 -> 返回「需要激活码」
json SteamEnrollAfterLogin() {
    std::wstring error;

    // 移除模式：登录完成后直接用撤销码移除令牌（不再调用 AddAuthenticator）
    std::string mode;
    std::string revocationCode;
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        mode = g_steamEnroll.mode;
        revocationCode = g_steamEnroll.revocationCode;
    }
    if (mode == "remove") {
        if (!SteamRemoveAuthenticator(revocationCode, error)) {
            return SteamEnrollErrorJson("remove_authenticator", error);
        }
        {
            std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
            g_steamEnroll.Clear();
        }
        SteamEnrollLog("remove ok");
        return json{ {"type", "steamEnrollDone"}, {"ok", true}, {"removed", true} };
    }

    if (!SteamAddAuthenticator(error)) {
        return SteamEnrollErrorJson("add_authenticator", error);
    }

    int confirmType = 0;
    std::string phoneHint;
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        confirmType = g_steamEnroll.confirmType;
        phoneHint = g_steamEnroll.phoneHint;
    }

    // confirm_type: 2 = 邮箱验证码，3 = 手机短信验证码
    std::string message = phoneHint;
    return SteamEnrollStepJson("activation", confirmType, message);
}

json SteamEnrollStartWorker(const std::string& username, const std::string& password,
                             const std::string& mode, const std::string& revocationCode) {
    SteamEnrollBusyGuard busyGuard;
    SteamEnrollLog("--- start: account=" + username + " mode=" + mode + " ---");
    std::wstring error;

    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        g_steamEnroll.Clear();
        g_steamEnroll.active = true;
        g_steamEnroll.accountName = username;
        g_steamEnroll.mode = mode;
        g_steamEnroll.revocationCode = revocationCode;
        g_steamEnroll.deviceId = MakeSteamDeviceId();
    }

    // 1) 取 RSA 公钥
    std::string modulus, exponent, encryptedPassword;
    uint64_t encryptionTimestamp = 0;
    if (!SteamGetPasswordRsaKey(username, modulus, exponent, encryptionTimestamp, error)) {
        return SteamEnrollErrorJson("rsa_key", error);
    }

    // 2) 加密密码
    if (!RsaEncryptPkcs1(modulus, exponent, password, encryptedPassword, error)) {
        return SteamEnrollErrorJson("rsa_encrypt", error);
    }

    // 3) 建立登录会话
    if (!SteamBeginAuthSession(username, encryptedPassword, encryptionTimestamp, error)) {
        return SteamEnrollErrorJson("begin_session", error);
    }

    int guardType = 0;
    std::string hint;
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        guardType = g_steamEnroll.guardType;
        hint = g_steamEnroll.phoneHint;
    }

    if (guardType == 3 || guardType == 2) {
        // 需要用户提供 Steam Guard 动态码 / 邮箱验证码
        return SteamEnrollStepJson("guard", guardType, hint);
    }
    if (guardType == 4 || guardType == 5) {
        // 需要在手机 App 上确认 / 点邮件链接，然后由前端触发轮询
        return SteamEnrollStepJson("confirm", guardType, hint);
    }

    // 无需验证码，直接轮询拿 token
    if (!SteamPollAuthSession(10, error)) {
        return SteamEnrollErrorJson("poll", error);
    }
    return SteamEnrollAfterLogin();
}

json SteamEnrollGuardCodeWorker(int codeType, const std::string& code) {
    SteamEnrollBusyGuard busyGuard;
    std::wstring error;

    if (!SteamSubmitGuardCode(codeType, code, error)) {
        return SteamEnrollErrorJson("guard_code", error);
    }
    if (!SteamPollAuthSession(10, error)) {
        return SteamEnrollErrorJson("poll", error);
    }
    return SteamEnrollAfterLogin();
}

json SteamEnrollPollWorker() {
    SteamEnrollBusyGuard busyGuard;
    std::wstring error;

    if (!SteamPollAuthSession(20, error)) {
        return SteamEnrollErrorJson("poll", error);
    }
    return SteamEnrollAfterLogin();
}

json SteamEnrollActivationWorker(const std::string& activationCode) {
    SteamEnrollBusyGuard busyGuard;
    std::wstring error;

    if (!SteamFinalizeAddAuthenticator(activationCode, error)) {
        return SteamEnrollErrorJson("finalize", error);
    }

    json response = {
        {"type", "steamEnrollDone"},
        {"ok", true}
    };
    {
        std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
        response["sharedSecret"] = g_steamEnroll.sharedSecret;       // base64 文本
        response["identitySecret"] = g_steamEnroll.identitySecret;
        response["revocationCode"] = g_steamEnroll.revocationCode;
        response["uri"] = g_steamEnroll.uri;
        response["accountName"] = g_steamEnroll.accountNameFromServer.empty()
            ? g_steamEnroll.accountName : g_steamEnroll.accountNameFromServer;
        response["steamId"] = std::to_string(g_steamEnroll.steamId);
        response["deviceId"] = g_steamEnroll.deviceId;
        g_steamEnroll.Clear();
    }
    return response;
}

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                     _In_opt_ HINSTANCE hPrevInstance,
                     _In_ LPWSTR    lpCmdLine,
                     _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    // 解决高分屏模糊：开启进程 DPI 感知
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // WebView2 依赖于 COM，必须初始化 COM 环境
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr))
    {
        // 使用转义字符表示：“COM 初始化失败”，“错误”
        MessageBoxW(nullptr, L"COM \u521d\u59cb\u5316\u5931\u8d25", L"\u9519\u8bef", MB_OK);
        return FALSE;
    }

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_ACCOUNTMANAGER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance (hInstance, nCmdShow))
    {
        return FALSE;
    }

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_ACCOUNTMANAGER));

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CoUninitialize();

    return (int) msg.wParam;
}

// 注册窗口类
ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style          = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc    = WndProc;
    wcex.cbClsExtra     = 0;
    wcex.cbWndExtra     = 0;
    wcex.hInstance      = hInstance;
    wcex.hIcon          = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ACCOUNTMANAGER));
    wcex.hCursor        = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground  = CreateSolidBrush(RGB(30, 30, 30)); // 深色主题背景 #1e1e1e
    wcex.lpszMenuName   = NULL; // Remove menu
    wcex.lpszClassName  = szWindowClass;
    wcex.hIconSm        = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    return RegisterClassExW(&wcex);
}
// 启动窗口拖动 / 缩放。
// 注意：这里必须用 PostMessage 而不是 SendMessage。
// SendMessage(hWnd, WM_NCLBUTTONDOWN, HTCAPTION/HTxxx, 0) 会立刻进入系统的
// 模态拖动/缩放循环，而本函数是在 WebView2 的 WebMessageReceived 回调里被调用的，
// 模态循环会重入派发消息给 WebView2 的窗口，触发其内部 CHECK -> int3
// （Debug 下表现为“已执行断点指令”，Release 下可能直接崩溃）。
// 改为投递消息，让拖动从正常消息循环启动；同时用真实光标位置作为拖动锚点，
// 避免窗口因 lParam 为 0 而跳位。
void StartWindowMoveOrResize(HWND hWnd, WPARAM hitTest) {
    POINT cursor = { 0, 0 };
    GetCursorPos(&cursor);
    ReleaseCapture();
    PostMessage(hWnd, WM_NCLBUTTONDOWN, hitTest, MAKELPARAM(cursor.x, cursor.y));
}

// 初始化窗口
BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
   hInst = hInstance;

   // 加载主题设置
   json initConfig = ReadConfigJson();
   if (initConfig.contains("theme")) {
       std::string theme = initConfig.value("theme", "dark");
       g_isDarkTheme = (theme != "light");
   }

   // 1. WS_OVERLAPPEDWINDOW + WM_NCCALCSIZE(return 0) 实现无边框 + 可缩放，且保留系统动画
   // WS_EX_APPWINDOW 确保在任务栏显示图标
   HWND hWnd = CreateWindowExW(WS_EX_APPWINDOW, szWindowClass, szTitle, 
      WS_OVERLAPPEDWINDOW,   // 含 WS_CAPTION：让 DWM 播放标准最小化/最大化动画；非客户区由 WM_NCCALCSIZE 去掉
      CW_USEDEFAULT, 0, w, h, nullptr, nullptr, hInstance, nullptr);

   if (!hWnd) return FALSE;

   // 2. 启用 DWM 阴影 (WS_POPUP 默认没阴影，需要手动开启)
   const MARGINS shadow_margins = { 1, 1, 1, 1 };
   DwmExtendFrameIntoClientArea(hWnd, &shadow_margins);

   // 获取程序目录
   WCHAR path[MAX_PATH];
   GetModuleFileNameW(NULL, path, MAX_PATH);
   std::filesystem::path exePath(path);
   std::wstring rootDir = exePath.parent_path().wstring();

   // 注册内联资源 Scheme 来处理 res:// 或者将 webview 拦截来处理内部资源
   // 不过为了方便，最快的方法仍然是将打包出来的那个单 HTML 文件在运行时写入到 Temp 目录再加载。
   
   // 为了单 EXE 发布，将 WebView2 的用户数据目录重定向到系统的临时目录 (Temp)
   WCHAR tempPath[MAX_PATH];
   GetTempPathW(MAX_PATH, tempPath);
   std::wstring userDataFolder = std::wstring(tempPath) + L"AccountManager_WebView2";

   // 释放内嵌的 HTML 到临时目录
   std::wstring htmlPath = userDataFolder + L"\\index.html";
   
   // 尝试从不同的资源类型名称加载
   HRSRC hResInfo = FindResourceW(hInstance, MAKEINTRESOURCEW(IDR_HTML1), L"HTML");
   if (!hResInfo) {
       hResInfo = FindResourceW(hInstance, MAKEINTRESOURCEW(IDR_HTML1), RT_HTML);
   }

   if (hResInfo) {
       HGLOBAL hResData = LoadResource(hInstance, hResInfo);
       if (hResData) {
           DWORD resSize = SizeofResource(hInstance, hResInfo);
           void* pData = LockResource(hResData);
           if (pData) {
               // 确保目录创建成功（如果是多级目录可能需要分别创建）
               CreateDirectoryW(userDataFolder.c_str(), NULL);
               
               HANDLE hFile = CreateFileW(htmlPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
               if (hFile != INVALID_HANDLE_VALUE) {
                   DWORD bytesWritten;
                   WriteFile(hFile, pData, resSize, &bytesWritten, NULL);
                   CloseHandle(hFile);
               } else {
                   MessageBoxW(NULL, L"无法释放 index.html 到 Temp 目录", L"文件写入错误", MB_OK);
                   return 0;
               }
           }
       } else {
           MessageBoxW(NULL, L"无法加载 HTML 资源", L"资源错误", MB_OK);
           return 0;
       }
   } else {
       MessageBoxW(NULL, L"找不到 IDR_HTML1 资源", L"资源错误", MB_OK);
       return 0;
   }

   // 初始化 WebView2
   HRESULT res = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
       Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
           [hWnd, rootDir, userDataFolder](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
               if (FAILED(result)) {
                   // 转义内容：“WebView2 环境创建失败: ”，“错误”
                   MessageBoxW(hWnd, (L"WebView2 \u73af\u5883\u521b\u5efa\u5931\u8d25: " + std::to_wstring(result)).c_str(), L"\u9519\u8bef", MB_OK);
                   return result;
               }

               env->CreateCoreWebView2Controller(hWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [hWnd, rootDir, userDataFolder](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                       if (FAILED(result) || controller == nullptr) {
                           // 转义内容：“WebView2 控制器创建失败: ”，“错误”
                           MessageBoxW(hWnd, (L"WebView2 \u63a7\u5236\u5668\u521b\u5efa\u5931\u8d25: " + std::to_wstring(result)).c_str(), L"\u9519\u8bef", MB_OK);
                           return result;
                       }

                       webviewController = controller;
                       webviewController->get_CoreWebView2(&webview);

                       // 彻底解决消息穿透问题：遍历并子类化 WebView2 下的所有子窗口
                       EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);

                       // 配置 WebView2
                       ICoreWebView2Settings* Settings;
                       webview->get_Settings(&Settings);
                       Settings->put_IsScriptEnabled(TRUE);
                       Settings->put_AreDefaultContextMenusEnabled(FALSE);
                       Settings->put_IsWebMessageEnabled(TRUE);

                       // 1. 拦截所有新窗口请求，改用系统默认浏览器打开
                       webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                           [](ICoreWebView2* sender, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                               wil::unique_cotaskmem_string uri;
                               args->get_Uri(&uri);
                               ShellExecuteW(NULL, L"open", uri.get(), NULL, NULL, SW_SHOWNORMAL);
                               args->put_Handled(TRUE); // 禁止 WebView 内部打开新窗口
                               return S_OK;
                           }).Get(), nullptr);

                       // 移除之前的禁用滚动手脚...

                       // 设置初始大小并显示 (透明边框模式，填满整个窗口)
                       RECT rc;
                       GetClientRect(hWnd, &rc);
                       int border = 0; // 透明边框模式：WebView2 填满整个窗口
                       RECT webviewRect = {
                           border,
                           border,
                           rc.right - border,
                           rc.bottom - border
                       };
                       webviewController->put_Bounds(webviewRect);
                       webviewController->put_IsVisible(TRUE);

                       // 映射虚拟域名到本地目录，解决 file:/// 协议的各类限制
                       wil::com_ptr<ICoreWebView2_3> webview3;
                       if (SUCCEEDED(webview->QueryInterface(IID_PPV_ARGS(&webview3)))) {
                           webview3->SetVirtualHostNameToFolderMapping(
                               L"app.assets", 
                               userDataFolder.c_str(), 
                               COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
                           );
                       }

                       // 注册消息处理
                       webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                           [hWnd](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                               wil::unique_cotaskmem_string messageRaw;
                               // 重要：前端发送的是对象，这里必须使用 get_WebMessageAsJson
                               args->get_WebMessageAsJson(&messageRaw);
                               if (!messageRaw) return S_OK;

                               try {
                                   std::wstring wmsg(messageRaw.get());
                                   std::string msgUtf8 = WStringToString(wmsg);
                                   json j = json::parse(msgUtf8);

                                   std::string type = j.value("type", "");

                                    // 1. 查询保险库状态（是否已初始化 / 是否已解锁 / 是否启用了 Windows Hello）
                                    if (type == "getVaultStatus") {
                                        json response = {
                                            {"type", "vaultStatus"},
                                            {"initialized", VaultFileExists()},
                                            {"unlocked", VaultIsUnlocked()},
                                            {"helloEnabled", HelloFileExists()},
                                            {"helloBusy", HelloIsBusy()}
                                        };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 1.1 用 Windows Hello 解锁（弹出 Windows 安全中心验证，工作线程执行）
                                    else if (type == "helloUnlock") {
                                        if (HelloIsBusy()) {
                                            json response = { {"type", "vaultError"}, {"code", "hello_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            HelloSetBusy(true);
                                            std::thread([hWnd]() {
                                                RunBackgroundWorker(hWnd, [] { return HelloUnlockWorker(); });
                                            }).detach();
                                        }
                                    }
                                    // 1.2 启用 Windows Hello 解锁（创建/打开 TPM 凭据并包装保险库密钥）
                                    else if (type == "helloEnable") {
                                        if (HelloIsBusy()) {
                                            json response = { {"type", "vaultError"}, {"code", "hello_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (!VaultIsUnlocked()) {
                                            json response = { {"type", "vaultError"}, {"code", "locked"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            std::vector<BYTE> aesKey;
                                            std::vector<BYTE> macKey;
                                            std::vector<BYTE> salt;
                                            ULONGLONG iterations = 0;
                                            bool unlocked = false;
                                            SnapshotVaultKeys(aesKey, macKey, salt, iterations, unlocked);
                                            HelloSetBusy(true);
                                            std::thread([hWnd, aesKey, macKey, salt, iterations]() {
                                                RunBackgroundWorker(hWnd, [aesKey, macKey, salt, iterations] {
                                                    return HelloEnableWorker(aesKey, macKey, salt, iterations);
                                                });
                                            }).detach();
                                        }
                                    }
                                    // 1.3 关闭 Windows Hello 解锁（删除 hello.json 与 Hello 凭据）
                                    else if (type == "helloDisable") {
                                        if (HelloIsBusy()) {
                                            json response = { {"type", "vaultError"}, {"code", "hello_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            HelloSetBusy(true);
                                            std::thread([hWnd]() {
                                                RunBackgroundWorker(hWnd, [] { return HelloDisableWorker(); });
                                            }).detach();
                                        }
                                    }
                                    // 2. 首次创建保险库（设置主密码），会顺带导入旧的 accounts.json
                                    else if (type == "createVault") {
                                        std::string password = j.value("masterPassword", "");
                                        if (password.size() < 6) {
                                            json response = { {"type", "vaultError"}, {"code", "password_too_short"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (VaultFileExists()) {
                                            json response = { {"type", "vaultError"}, {"code", "vault_exists"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            json entries = MigrateLegacyAccounts();
                                            json categories = j.value("categories", json::array());
                                            int imported = (int)entries.size();
                                            std::wstring err;
                                            if (VaultCreate(password, entries, categories, err)) {
                                                json response = {
                                                    {"type", "vaultUnlocked"},
                                                    {"entries", entries},
                                                    {"categories", categories},
                                                    {"importedCount", imported}
                                                };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                            else {
                                                json response = {
                                                    {"type", "vaultError"},
                                                    {"code", "create_failed"},
                                                    {"detail", WStringToString(err)}
                                                };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 3. 解锁保险库
                                    else if (type == "unlockVault") {
                                        std::string password = j.value("masterPassword", "");
                                        json entries = json::array();
                                        json categories = json::array();
                                        std::string code;
                                        if (VaultUnlock(password, entries, categories, code)) {
                                            json response = {
                                                {"type", "vaultUnlocked"},
                                                {"entries", entries},
                                                {"categories", categories}
                                            };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            json response = { {"type", "vaultError"}, {"code", code} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                    }
                                    // 4. 保存条目（自动加密后写入 vault.json）
                                    else if (type == "saveVault") {
                                        if (!VaultIsUnlocked()) {
                                            json response = { {"type", "vaultError"}, {"code", "locked"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (j.contains("entries")) {
                                            std::wstring err;
                                            if (VaultSave(j["entries"], j.value("categories", json::array()), err)) {
                                                json response = { {"type", "vaultSaved"} };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                            else {
                                                json response = {
                                                    {"type", "vaultError"},
                                                    {"code", "save_failed"},
                                                    {"detail", WStringToString(err)}
                                                };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 5. 锁定保险库（清空内存密钥）
                                    else if (type == "lockVault") {
                                        VaultLock();
                                        json response = { {"type", "vaultLocked"} };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 6. 修改主密码
                                    else if (type == "changeMasterPassword") {
                                        std::string current = j.value("currentPassword", "");
                                        std::string next = j.value("newPassword", "");
                                        if (next.size() < 6) {
                                            json response = { {"type", "vaultError"}, {"code", "password_too_short"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            json entries = json::array();
                                            std::string code;
                                            if (VaultChangeMasterPassword(current, next, entries, code)) {
                                                // 密钥已更换：若启用了 Hello 解锁，需要在工作线程里用新密钥重新包装
                                                std::vector<BYTE> aesKey;
                                                std::vector<BYTE> macKey;
                                                std::vector<BYTE> salt;
                                                ULONGLONG iterations = 0;
                                                bool unlocked = false;
                                                SnapshotVaultKeys(aesKey, macKey, salt, iterations, unlocked);
                                                HelloSetBusy(true);
                                                std::thread([hWnd, aesKey, macKey, salt, iterations]() {
                                                    RunBackgroundWorker(hWnd, [aesKey, macKey, salt, iterations] {
                                                        return HelloRewrapWorker(aesKey, macKey, salt, iterations);
                                                    });
                                                }).detach();
                                            }
                                            else {
                                                json response = { {"type", "vaultError"}, {"code", code} };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 7. 处理获取配置请求
                                    else if (type == "getConfig") {
                                        json config = ReadConfigJson();
                                        json response = {
                                            {"type", "setConfig"},
                                            {"config", config}
                                        };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 8. 处理保存配置请求
                                    else if (type == "saveConfig") {
                                        if (j.contains("config")) {
                                            SaveConfigJson(j["config"]);
                                            // 更新主题设置并重绘窗口边框
                                            if (j["config"].contains("theme")) {
                                                std::string theme = j["config"]["theme"].get<std::string>();
                                                bool newIsDark = (theme != "light");
                                                if (g_isDarkTheme != newIsDark) {
                                                    g_isDarkTheme = newIsDark;
                                                    // 重绘窗口边框
                                                    RECT rc;
                                                    GetClientRect(hWnd, &rc);
                                                    InvalidateRect(hWnd, &rc, TRUE);
                                                }
                                            }
                                        }
                                    }
                                    // 9. 复制文本到剪贴板（避免依赖浏览器剪贴板权限）
                                    else if (type == "copyText") {
                                        std::string text = j.value("text", "");
                                        if (!text.empty()) {
                                            std::wstring wText = StringToWString(text);
                                            if (OpenClipboard(hWnd)) {
                                                EmptyClipboard();
                                                size_t bytes = (wText.size() + 1) * sizeof(wchar_t);
                                                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
                                                if (hMem) {
                                                    void* dst = GlobalLock(hMem);
                                                    if (dst) {
                                                        memcpy(dst, wText.c_str(), bytes);
                                                        GlobalUnlock(hMem);
                                                        SetClipboardData(CF_UNICODETEXT, hMem);
                                                    }
                                                }
                                                CloseClipboard();
                                                json response = { {"type", "clipboardCopied"} };
                                                std::wstring wResponse = StringToWString(response.dump());
                                                webview->PostWebMessageAsJson(wResponse.c_str());
                                            }
                                        }
                                    }
                                    // 10. 处理打开外部链接请求
                                    else if (type == "openUrl") {
                                        std::string url = j.value("url", "");
                                        if (!url.empty()) {
                                            ShellExecuteW(NULL, L"open", StringToWString(url).c_str(), NULL, NULL, SW_SHOWNORMAL);
                                        }
                                    }
                                    // 11. 打开本地程序 / 文件（软件类条目）
                                    else if (type == "openPath") {
                                        std::string target = j.value("path", "");
                                        if (!target.empty()) {
                                            ShellExecuteW(NULL, L"open", StringToWString(target).c_str(), NULL, NULL, SW_SHOWNORMAL);
                                        }
                                    }
                                    // 11.5 从本机 Steam 导入已登录账号（只读 loginusers.vdf）
                                    else if (type == "steamImportAccounts") {
                                        std::wstring importError;
                                        json accounts = ReadSteamLoginUsers(importError);
                                        json response = {
                                            {"type", "steamAccounts"},
                                            {"accounts", accounts},
                                            {"error", WStringToString(importError)}
                                        };
                                        std::wstring wResponse = StringToWString(response.dump());
                                        webview->PostWebMessageAsJson(wResponse.c_str());
                                    }
                                    // 12. Steam 一键登录（关闭 Steam -> 写自动登录 -> 启动，工作线程执行）
                                    else if (type == "steamLogin") {
                                        std::string username = j.value("username", "");
                                        std::string password = j.value("password", "");
                                        if (username.empty()) {
                                            json response = { {"type", "steamLoginResult"}, {"ok", false}, {"code", "steam_no_username"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else if (g_steamBusy) {
                                            json response = { {"type", "steamLoginResult"}, {"ok", false}, {"code", "steam_busy"} };
                                            std::wstring wResponse = StringToWString(response.dump());
                                            webview->PostWebMessageAsJson(wResponse.c_str());
                                        }
                                        else {
                                            g_steamBusy = true;
                                            std::wstring wUser = StringToWString(username);
                                            std::wstring wPass = StringToWString(password);
                                            std::thread([hWnd, wUser, wPass]() {
                                                RunBackgroundWorker(hWnd, [wUser, wPass] {
                                                    return SteamLoginWorker(wUser, wPass);
                                                });
                                            }).detach();
                                        }
                                    }
                                   // 13. Steam 令牌登记（应用内添加手机令牌，工作线程执行）
                                   else if (type == "steamEnrollStart") {
                                       std::string username = j.value("username", "");
                                       std::string password = j.value("password", "");
                                       std::string mode = j.value("mode", "add");
                                       std::string revocationCode = j.value("revocationCode", "");
                                       if (username.empty() || password.empty()) {
                                           json response = SteamEnrollErrorJson("missing_credentials", L"账号或密码为空");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else if (g_steamEnrollBusy) {
                                           json response = SteamEnrollErrorJson("busy", L"已有令牌操作正在进行");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd, username, password, mode, revocationCode]() {
                                               RunBackgroundWorker(hWnd, [username, password, mode, revocationCode] {
                                                   return SteamEnrollStartWorker(username, password, mode, revocationCode);
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollGuardCode") {
                                       std::string code = j.value("code", "");
                                       int codeType = j.value("codeType", 3);
                                       if (code.empty()) {
                                           json response = SteamEnrollErrorJson("missing_code", L"验证码为空");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else if (!g_steamEnrollBusy) {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd, codeType, code]() {
                                               RunBackgroundWorker(hWnd, [codeType, code] {
                                                   return SteamEnrollGuardCodeWorker(codeType, code);
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollPoll") {
                                       if (!g_steamEnrollBusy) {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd]() {
                                               RunBackgroundWorker(hWnd, [] {
                                                   return SteamEnrollPollWorker();
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollActivation") {
                                       std::string code = j.value("code", "");
                                       if (code.empty()) {
                                           json response = SteamEnrollErrorJson("missing_code", L"激活码为空");
                                           std::wstring wResponse = StringToWString(response.dump());
                                           webview->PostWebMessageAsJson(wResponse.c_str());
                                       }
                                       else if (!g_steamEnrollBusy) {
                                           g_steamEnrollBusy = true;
                                           std::thread([hWnd, code]() {
                                               RunBackgroundWorker(hWnd, [code] {
                                                   return SteamEnrollActivationWorker(code);
                                               });
                                           }).detach();
                                       }
                                   }
                                   else if (type == "steamEnrollCancel") {
                                       {
                                           std::lock_guard<std::mutex> lock(g_steamEnrollMutex);
                                           g_steamEnroll.Clear();
                                       }
                                       g_steamEnrollBusy = false;
                                       json response = { {"type", "steamEnrollCancelled"} };
                                       std::wstring wResponse = StringToWString(response.dump());
                                       webview->PostWebMessageAsJson(wResponse.c_str());
                                   }
                                   // 5. 窗口控制指令
                                   else if (type == "windowControl") {
                                       std::string action = j.value("action", "");
                                       if (action == "minimize") {
                                           PostMessage(hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);   // 走系统命令以保留最小化动画
                                       }
                                       else if (action == "maximize") {
                                           if (IsZoomed(hWnd)) PostMessage(hWnd, WM_SYSCOMMAND, SC_RESTORE, 0);   // 走系统命令以保留动画
                                           else PostMessage(hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);   // 走系统命令以保留动画
                                       }
                                       else if (action == "close") {
                                           PostMessage(hWnd, WM_CLOSE, 0, 0);
                                       }
                                       else if (action == "startDrag") {
                                           // 核心：请求系统拖拽（投递消息启动，勿在 WebView2 回调里同步调用）
                                                                                      StartWindowMoveOrResize(hWnd, HTCAPTION);
                                       }
                                       // 窗口缩放控制
                                       else if (action == "startResizeLeft") {
                                                                                      StartWindowMoveOrResize(hWnd, HTLEFT);
                                       }
                                       else if (action == "startResizeRight") {
                                                                                      StartWindowMoveOrResize(hWnd, HTRIGHT);
                                       }
                                       else if (action == "startResizeTop") {
                                                                                      StartWindowMoveOrResize(hWnd, HTTOP);
                                       }
                                       else if (action == "startResizeBottom") {
                                                                                      StartWindowMoveOrResize(hWnd, HTBOTTOM);
                                       }
                                       else if (action == "startResizeTopLeft") {
                                                                                      StartWindowMoveOrResize(hWnd, HTTOPLEFT);
                                       }
                                       else if (action == "startResizeTopRight") {
                                                                                      StartWindowMoveOrResize(hWnd, HTTOPRIGHT);
                                       }
                                       else if (action == "startResizeBottomLeft") {
                                                                                      StartWindowMoveOrResize(hWnd, HTBOTTOMLEFT);
                                       }
                                       else if (action == "startResizeBottomRight") {
                                                                                      StartWindowMoveOrResize(hWnd, HTBOTTOMRIGHT);
                                       }
                                   }
                               }
                               catch (const std::exception& e) {
                                   OutputDebugStringA(e.what());
                               }

                               return S_OK;
                           }).Get(), nullptr);

                       // 导航到页面 (使用资源协议加载内部打包的 html)
                       // 映射虚拟域名到虚拟的资源路径
                       webview->Navigate(L"https://app.assets/index.html");

                       return S_OK;
                   }).Get());
               return S_OK;
           }).Get());

   if (FAILED(res)) {
       // 转义内容：“WebView2 初始化调用失败，请检查是否安装了 WebView2 Runtime”，“错误”
       MessageBoxW(hWnd, L"WebView2 \u521d\u59cb\u5316\u8c03\u7528\u5931\u8d25\uff0c\u8bf7\u68c0\u67e5\u662f\u5426\u5b89\u88c5\u4e86 WebView2 Runtime", L"\u9519\u8bef", MB_OK);
   }

   ShowWindow(hWnd, nCmdShow);
   UpdateWindow(hWnd);

   return TRUE;
}
// 窗口过程函数
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    // 处理窗口消息
    switch (message)
    {
    //  绘制窗口背景（透明边框版本）
    case WM_ERASEBKGND:
    {
        HDC hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hWnd, &rc);
        
        // 根据主题选择背景颜色（匹配 Vue CSS 变量）
        COLORREF backColor = g_isDarkTheme ? RGB(30, 30, 30) : RGB(243, 243, 243);    // 深色: #1e1e1e, 浅色: #f3f3f3
        HBRUSH hBackBrush = CreateSolidBrush(backColor);
        
        // 填充整个客户区为背景色，确保在WebView2加载前显示统一背景
        FillRect(hdc, &rc, hBackBrush);
        
        DeleteObject(hBackBrush);
        return 1;
    }
    // 处理鼠标移动消息
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc;
        GetClientRect(hWnd, &rc);

        int windowControlsLeft = rc.right - 140;
        bool isOverControls = (pt.x >= windowControlsLeft && pt.y <= 32);

        if (isOverControls != g_isHoveringWindowControls) {
            g_isHoveringWindowControls = isOverControls;
            RECT hoverRect = { rc.right - 140, 0, rc.right, 4 };
            InvalidateRect(hWnd, &hoverRect, FALSE);
        }

        if (!g_isTrackingMouse) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_isTrackingMouse = true;
        }
        break;
    }
    // 处理鼠标离开消息
    case WM_MOUSELEAVE:
    {
        g_isTrackingMouse = false;
        if (g_isHoveringWindowControls) {
            g_isHoveringWindowControls = false;
            RECT rc;
            GetClientRect(hWnd, &rc);
            RECT hoverRect = { rc.right - 140, 0, rc.right, 4 };
            InvalidateRect(hWnd, &hoverRect, FALSE);
        }
        break;
    }
    // 处理窗口激活消息
    case WM_NCACTIVATE:
        // 阻止系统在激活/非激活时绘制默认标题栏/边框
        return TRUE;
    // 处理鼠标双击消息
    case WM_LBUTTONDBLCLK:
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        // 注意：WM_LBUTTONDBLCLK 的坐标是 client 坐标，无需转换
        RECT rc;
        GetClientRect(hWnd, &rc);
        const int titleHeight = 32;

        // 如果双击在标题栏区域（且避开了右侧控制按钮），执行最大化/还原
        if (pt.y < titleHeight && pt.x < rc.right - 140) {
            if (IsZoomed(hWnd)) PostMessage(hWnd, WM_SYSCOMMAND, SC_RESTORE, 0);
            else PostMessage(hWnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
            return 0;
        }
        break;
    }
    // 处理窗口大小调整消息
    case WM_NCCALCSIZE:
        // WS_POPUP 模式下不需要特殊处理 NC 区域，直接返回 0 即可
        return 0;
    // 处理窗口大小调整消息
    case WM_NCHITTEST:
    {
        // 前端检测鼠标位置并处理窗口缩放，此处始终返回HTCLIENT让前端处理
        return HTCLIENT;
    }
    // 处理窗口大小调整消息
    case WM_GETMINMAXINFO:
    {
        LPMINMAXINFO lpMMI = (LPMINMAXINFO)lParam;
        // 设置最小可拖拽尺寸
        lpMMI->ptMinTrackSize.x = 1100;
        lpMMI->ptMinTrackSize.y = 830;
        
        // 关键：处理 WS_POPUP 模式下的最大化溢出
        // 获取窗口所在的当前显示器
        HMONITOR hMonitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
        if (hMonitor != NULL) {
            MONITORINFO mi = { sizeof(mi) };
            if (GetMonitorInfo(hMonitor, &mi)) {
                // ptMaxPosition 是相对于系统主显示器 (0,0) 的偏移
                // 所以不需要减去 rcMonitor.left，只需计算屏幕左上角相对工作区左上角的偏移
                // 在多显示器且不同分辨率下，正确设置偏移非常重要
                lpMMI->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                lpMMI->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                
                // 设置最大化时的宽高为工作区 (rcWork) 的宽高
                lpMMI->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                lpMMI->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
                
                // 限制最大可拖拽范围
                lpMMI->ptMaxTrackSize.x = lpMMI->ptMaxSize.x;
                lpMMI->ptMaxTrackSize.y = lpMMI->ptMaxSize.y;
            }
        }
        return 0; // 必须返回 0 表示已处理
    }
    // 处理窗口大小调整消息
    case WM_SIZE:
        if (webviewController != nullptr) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            
            // 透明边框模式：WebView2 填满整个窗口，缩放通过 WM_NCHITTEST 处理
            int border = 0;
            
            RECT webviewRect = {
                border,
                border,
                rc.right - border,
                rc.bottom - border
            };
            webviewController->put_Bounds(webviewRect);
            
            // 关键：重新遍历子窗口
            EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);
        }
        break;
    // 处理进入缩放/移动模式
    case WM_ENTERSIZEMOVE:
        // 缩放开始时重新子类化所有子窗口
        if (webviewController != nullptr) {
            EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);
        }
        break;
    // 处理退出缩放/移动模式
    case WM_EXITSIZEMOVE:
        // 缩放结束后重新子类化所有子窗口
        if (webviewController != nullptr) {
            EnumChildWindows(hWnd, EnumChildProc, (LPARAM)hWnd);
        }
        break;
    // 处理后台工作线程（Windows Hello / Steam 登录）的结果：回到 UI 线程再发给前端
    case WM_WORKER_RESULT:
    {
        HelloSetBusy(false);
        g_steamBusy = false;
        std::string* payload = reinterpret_cast<std::string*>(lParam);
        if (payload) {
            // 窗口过程里不能让异常逃出去
            try {
                if (webview) {
                    std::wstring wResponse = StringToWString(*payload);
                    webview->PostWebMessageAsJson(wResponse.c_str());

                    // Windows Hello 的系统验证弹窗关闭后，窗口不一定重新获得键盘焦点。
                    // 主动夺回前台与键盘焦点，并让 WebView2 把焦点移进网页内容；
                    // 否则前端只能改 document.activeElement，用户打字仍进不去。
                    if (payload->find("hello_") != std::string::npos) {
                        SetForegroundWindow(hWnd);
                        SetFocus(hWnd);
                        if (webviewController) {
                            webviewController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                        }
                    }
                }
            }
            catch (...) {
                OutputDebugStringW(L"[Worker] failed to deliver result\n");
            }
            delete payload;
        }
        return 0;
    }
    // 处理窗口销毁消息
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}
