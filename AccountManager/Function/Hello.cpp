// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Hello.cpp —— Windows Hello 解锁（Windows 安全中心验证：PIN / 指纹 / 人脸）
//   启用：创建或打开 TPM 保护的 Hello 密钥凭据 -> 对固定挑战签名 ->
//         用 HKDF-SHA256(签名) 派生包装密钥 -> 用它加密
//         {AES 密钥, HMAC 密钥, 盐, 迭代次数} 写入 AccountManager.json 的 hello 节。
//         私钥由 TPM 保护，必须通过 PIN / 指纹 / 人脸验证才能完成签名。
//   解锁：读取 hello 数据 -> 签名挑战（弹出 Windows 安全中心验证）->
//         解出保险库密钥 -> 用其解密保险库，无需输入主密码。
//   注意：修改主密码会更换保险库密钥，因此需要重新包装一次 Hello 数据，
//         否则 Hello 解锁会失效（此时仍可用主密码解锁）。

#include "Function.h"

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
