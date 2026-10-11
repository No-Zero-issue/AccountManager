// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// SteamAuth.cpp —— Steam 账号登录 + 应用内添加手机令牌
//   全部走 Steam 官方接口：
//     1. IAuthenticationService/GetPasswordRSAPublicKey（必须 GET）
//     2. Windows CNG 做 RSA PKCS#1 v1.5 加密密码 -> BeginAuthSessionViaCredentials
//     3. 邮箱验证码 / 现有令牌动态码 / 手机 App 确认 -> UpdateAuthSessionWithSteamGuardCode
//     4. PollAuthSessionStatus 轮询拿 access / refresh token
//     5. ITwoFactorService/AddAuthenticator 下发 shared_secret / identity_secret / revocation_code
//     6. FinalizeAddAuthenticator 用短信 / 邮箱激活码完成激活
//        （必须同时带上用新密钥算出的 authenticator_code，且 authenticator_time 用服务端时间）
//     7. ITwoFactorService/RemoveAuthenticator 移除令牌
//   排查：登记过程会在 EXE 同目录写 steam_enroll.log（不含密码、密钥、验证码）。

#include "Function.h"

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
