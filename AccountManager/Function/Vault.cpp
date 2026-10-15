// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Vault.cpp —— 密码保险库
//   KDF    : PBKDF2-HMAC-SHA256 (BCryptDeriveKeyPBKDF2, 200000 次迭代)
//   加密   : AES-256-CBC + PKCS7 填充 (BCryptEncrypt / BCryptDecrypt)
//   完整性 : HMAC-SHA256(salt || iv || ciphertext)，先加密后校验
//   结构   : { version, kdf:{algorithm,iterations,salt}, cipher, iv, mac, data }
//   说明   : 解密用的密钥只在解锁期间驻留内存，锁定/退出时清零；
//            解锁后的密钥供 Hello 复用，因此读写密钥都加锁。

#include "Function.h"

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


bool VaultFileExists() {
    std::error_code ec;
    std::filesystem::path path = GetVaultFilePath();
    if (std::filesystem::exists(path, ec) && std::filesystem::file_size(path, ec) > 0) return true;
    // 合并存储后保险库写在 AccountManager.json 的 vault 节（ReadDataSection 会先迁移旧 vault.json）。
    // 这里必须查 vault 节：查 hello 节会让只用主密码、没开 Windows Hello 的用户被误判成首次运行。
    json section = ReadDataSection("vault");
    return section.is_object() && section.contains("kdf") && section.contains("data");
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
