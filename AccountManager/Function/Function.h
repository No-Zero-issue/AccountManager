// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Function.h —— 内部共享头（只在本项目内部使用，不对外暴露）
//   原来全部代码挤在 AccountManager.cpp（3700 行）里，现按功能拆到 Function/ 目录：
//     Utils.cpp        字符编码 / Base64 / Hex / 随机数
//     Crypto.cpp       CNG 密码学原语：PBKDF2 / AES-256-CBC / HMAC-SHA256 / HKDF / RSA
//     DataStore.cpp    配置与单文件 AccountManager.json 的分区读写、旧文件迁移
//     Vault.cpp        密码保险库：密钥驻留 + 信封 + 创建/解锁/保存/改密
//     Hello.cpp        Windows Hello 解锁（Windows 安全中心验证）
//     Proto.cpp        极简 protobuf 编解码（namespace steampb）
//     Http.cpp         WinHTTP 传输层
//     Vdf.cpp          VDF 解析 + 从本机 Steam 导入账号
//     SteamClient.cpp  Steam 客户端一键登录（注册表 + 进程）
//     SteamAuth.cpp    Steam 账号登录 + 应用内手机令牌
//   AccountManager.cpp 只保留：程序入口、窗口过程、WebView2 宿主、前端消息分发。
//   本头文件集中放「跨文件」的东西：公共 include、宏、常量、结构体、全局量、函数声明。

#pragma once

#include "framework.h"
#include "AccountManager.h"
#include "version.h"   // 程序版本（唯一来源）：APP_VERSION_LABEL / APP_USER_AGENT 等
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

// 后台工作线程完成后向 UI 线程投递结果的自定义消息
#define WM_WORKER_RESULT (WM_APP + 1)

// ---- cross-file constants ----
constexpr ULONGLONG kVaultKdfIterations = 200000;
constexpr size_t kVaultAesKeySize = 32;
constexpr size_t kVaultMacKeySize = 32;
constexpr size_t kVaultSaltSize = 16;
constexpr size_t kVaultIvSize = 16;

// ---- cross-file structs ----
struct SteamHttpResult {
    bool ok = false;
    DWORD statusCode = 0;
    std::string body;
    std::wstring error;
};

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

// ---- cross-file globals (defined in their own module) ----
extern std::atomic<bool> g_steamBusy;        // SteamClient.cpp
extern std::atomic<bool> g_steamEnrollBusy;  // SteamAuth.cpp
extern SteamEnrollSession g_steamEnroll;     // SteamAuth.cpp
extern std::mutex g_steamEnrollMutex;       // SteamAuth.cpp

// ---- cross-file function declarations ----
std::string WStringToString(const std::wstring& text);
std::wstring StringToWString(const std::string& text);
std::wstring GetSteamPath();
bool SetAutoLoginUser(const std::wstring& username);
bool IsSteamRunning();
void KillSteam();
bool StartSteam(const std::wstring& username, const std::wstring& password, std::wstring& error);
json SteamLoginWorker(std::wstring username, std::wstring password);
std::wstring GetConfigFilePath();
json ReadConfigJson();
void SaveConfigJson(const json& j);
void WipeBytes(std::vector<BYTE>& buf);
void WipeText(std::string& text);
void InstallVaultKeys(const std::vector<BYTE>& aesKey, const std::vector<BYTE>& macKey, const std::vector<BYTE>& salt, ULONGLONG iterations);
void SnapshotVaultKeys(std::vector<BYTE>& aesKey, std::vector<BYTE>& macKey, std::vector<BYTE>& salt, ULONGLONG& iterations, bool& unlocked);
bool RandomBytes(std::vector<BYTE>& out, size_t count);
std::string Base64Encode(const std::vector<BYTE>& data);
std::vector<BYTE> Base64Decode(const std::string& text);
bool DeriveVaultKeys(const std::string& password, const std::vector<BYTE>& salt, ULONGLONG iterations, std::vector<BYTE>& aesKey, std::vector<BYTE>& macKey);
bool AesCbcEncrypt(const std::vector<BYTE>& key, const std::vector<BYTE>& iv, const std::string& plain, std::vector<BYTE>& cipher);
bool AesCbcDecrypt(const std::vector<BYTE>& key, const std::vector<BYTE>& iv, const std::vector<BYTE>& cipher, std::vector<BYTE>& plain);
bool ComputeHmacSha256(const std::vector<BYTE>& key, const std::vector<BYTE>& data, std::vector<BYTE>& mac);
bool ConstantTimeEquals(const std::vector<BYTE>& a, const std::vector<BYTE>& b);
std::wstring GetVaultFilePath();
std::wstring GetLegacyAccountsFilePath();
bool BuildVaultJson(const std::vector<BYTE>& aesKey, const std::vector<BYTE>& macKey, const std::vector<BYTE>& salt, ULONGLONG iterations, const json& payload, json& outFile);
bool WriteVaultFile(const json& fileData, std::wstring& error);
bool ReadVaultEnvelope(json& fileData, std::vector<BYTE>& salt, std::vector<BYTE>& iv, std::vector<BYTE>& mac, std::vector<BYTE>& cipher, ULONGLONG& iterations, std::string& errorCode);
bool DecryptEnvelopeWithKeys(const std::vector<BYTE>& aesKey, const std::vector<BYTE>& macKey, const std::vector<BYTE>& salt, const std::vector<BYTE>& iv, const std::vector<BYTE>& mac, const std::vector<BYTE>& cipher, const char* macErrorCode, json& payload, std::string& errorCode);
bool DecryptVaultFile(const std::string& password, json& payload, std::string& errorCode);
bool DecryptVaultWithStoredKeys(json& payload, std::string& errorCode);
bool VaultFileExists();
json MigrateLegacyAccounts();
bool VaultCreate(const std::string& password, const json& entries, const json& categories, std::wstring& error);
bool VaultUnlock(const std::string& password, json& entries, json& categories, std::string& errorCode);
bool VaultSave(const json& entries, const json& categories, std::wstring& error);
void VaultLock();
bool VaultIsUnlocked();
bool VaultChangeMasterPassword(const std::string& currentPassword, const std::string& newPassword, json& entries, std::string& errorCode);
std::wstring GetHelloFilePath();
std::wstring GetDataFilePath();
json ReadDataRoot();
bool WriteDataRoot(const json& root);
json ReadDataSection(const std::string& name);
bool WriteDataSection(const std::string& name, const json& value);
bool RemoveDataSection(const std::string& name);
void MigrateLegacyDataFiles();
void RemoveHelloFile();
bool HkdfSha256(const std::vector<BYTE>& ikm, const std::string& salt, const std::string& info, std::vector<BYTE>& out);
bool ReadHelloFile(json& out, std::string& errorCode);
bool HelloSignChallenge(const std::vector<BYTE>& challenge, bool createIfMissing, std::vector<BYTE>& signature, std::string& errorCode);
bool HelloFileExists();
bool HelloIsBusy();
void HelloSetBusy(bool busy);
bool HelloWrapKeyBlob(const std::vector<BYTE>& signature, const std::vector<BYTE>& challenge, const json& keyBlob, json& outFile);
bool HelloUnwrapKeyBlob(const std::vector<BYTE>& signature, const std::vector<BYTE>& challenge, const json& fileData, json& keyBlob, std::string& errorCode);
json HelloEnableWorker(std::vector<BYTE> aesKey, std::vector<BYTE> macKey, std::vector<BYTE> salt, ULONGLONG iterations);
json HelloRewrapWorker(std::vector<BYTE> aesKey, std::vector<BYTE> macKey, std::vector<BYTE> salt, ULONGLONG iterations);
json HelloUnlockWorker();
json HelloDisableWorker();
std::string UrlEncodeForm(const std::string& text);
bool SteamHttpsRequest(bool useGet, const std::wstring& host, const std::wstring& path, const std::string& formBody, SteamHttpResult& result);
bool SteamHttpsPost(const std::wstring& host, const std::wstring& path, const std::string& formBody, SteamHttpResult& result);
bool SteamHttpsGet(const std::wstring& host, const std::wstring& path, SteamHttpResult& result);
uint64_t UnixTimeNow();
bool SteamQueryTime(uint64_t& serverTime, uint64_t& localTime, std::wstring& error);
json ReadSteamLoginUsers(std::wstring& error);
std::vector<BYTE> HexToBytes(const std::string& hex);
std::string BytesToHex(const std::vector<BYTE>& bytes);
std::string BytesToBase64(const std::vector<BYTE>& bytes);
std::string Base64ToBytesString(const std::string& base64);
bool RsaEncryptPkcs1(const std::string& modulusHex, const std::string& exponentHex, const std::string& plaintext, std::string& cipherBase64, std::wstring& error);
bool SteamUnifiedCall(const std::wstring& method, const std::string& protoPayload, const std::wstring& accessToken, const std::string& routingQuery, std::string& responseBody, std::wstring& error);
bool SteamResponseValue(const std::string& body, const std::string& jsonField, uint32_t protoField, uint32_t wireType, std::string& bytesOut, uint64_t& varintOut);
std::string MakeSteamDeviceId();
void SteamEnrollLog(const std::string& text);
bool SteamGetPasswordRsaKey(const std::string& accountName, std::string& modulus, std::string& exponent, uint64_t& timestamp, std::wstring& error);
bool SteamBeginAuthSession(const std::string& accountName, const std::string& password, uint64_t encryptionTimestamp, std::wstring& error);
bool SteamSubmitGuardCode(int codeType, const std::string& code, std::wstring& error);
bool SteamPollAuthSession(int maxAttempts, std::wstring& error);
bool SteamAddAuthenticator(std::wstring& error);
std::string SteamGuardCodeForTime(const std::string& sharedSecretBase64, uint64_t unixSeconds);
bool SteamRemoveAuthenticator(const std::string& revocationCode, std::wstring& error);
bool SteamFinalizeAddAuthenticator(const std::string& activationCode, std::wstring& error);
json SteamEnrollErrorJson(const std::string& code, const std::wstring& detail);
json SteamEnrollStepJson(const std::string& step, int codeType, const std::string& message);
json SteamEnrollAfterLogin();
json SteamEnrollStartWorker(const std::string& username, const std::string& password, const std::string& mode, const std::string& revocationCode);
json SteamEnrollGuardCodeWorker(int codeType, const std::string& code);
json SteamEnrollPollWorker();
json SteamEnrollActivationWorker(const std::string& activationCode);

// ---- minimal protobuf (Proto.cpp, namespace steampb) ----
namespace steampb {
void WriteVarint(std::string& out, uint64_t value);
void WriteUInt64Field(std::string& out, uint32_t field, uint64_t value);
void WriteBytesField(std::string& out, uint32_t field, const std::string& value);
bool ReadVarint(const std::string& data, size_t& pos, uint64_t& value);
bool FindField(const std::string& data, uint32_t wantedField, uint32_t wantedWireType, std::string& bytesOut, uint64_t& varintOut);
bool FindVarint(const std::string& data, uint32_t field, uint64_t& value);
bool FindBytes(const std::string& data, uint32_t field, std::string& value);
void WriteFixed64Field(std::string& out, uint32_t field, uint64_t value);
}

// ---- background worker wrapper (template, header-only) ----
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

