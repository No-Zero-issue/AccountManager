// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Crypto.cpp —— CNG 密码学原语
//   PBKDF2-HMAC-SHA256 派生、AES-256-CBC 加解密、HMAC-SHA256、HKDF-SHA256、RSA PKCS#1 v1.5。
//   全部走 Windows CNG（bcrypt），不引入第三方密码学库。

#include "Function.h"

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
