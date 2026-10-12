// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Account Manager 开发者
//
// Http.cpp —— WinHTTP 传输层
//   注意：Steam 各接口要求的 HTTP 方法不同（例如 GetPasswordRSAPublicKey 必须 GET，
//   用 POST 会返回 405「This API must be called with a HTTP GET request」）。

#include "Function.h"

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
// HTTPS 请求（GET / POST）
//   注意：Steam 各接口要求的 HTTP 方法不同（例如 GetPasswordRSAPublicKey 必须 GET，
//   方法不对会返回 405「This API must be called with a HTTP GET request」）。
bool SteamHttpsRequest(bool useGet, const std::wstring& host, const std::wstring& path,
                       const std::string& formBody, SteamHttpResult& result) {
    result = SteamHttpResult{};

    // User-Agent 里的版本号来自 version.h，随程序版本自动变化
    HINTERNET session = WinHttpOpen(APP_USER_AGENT,
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


