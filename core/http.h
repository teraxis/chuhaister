// Synchronous HTTPS client on WinHTTP for the cswap native core.
// One call — one request; no connection reuse (calls are rare: token refresh,
// profile, usage). TLS is WinHTTP's, so system proxy/cert store apply.

#pragma once

#include "win_util.h"

#include <winhttp.h>

#include <string>
#include <vector>

namespace cswap {

struct HttpResponse {
    bool ok = false;       // transport-level success (a 4xx/5xx still sets ok)
    DWORD status = 0;
    std::string body;
    std::wstring error;    // transport failure description when !ok
};

inline HttpResponse httpsRequest(const std::wstring& method, const std::wstring& url,
                                 const std::vector<std::wstring>& headers,
                                 const std::string& body, DWORD timeoutMs = 15000) {
    HttpResponse r;

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        r.error = L"bad url";
        return r;
    }

    HINTERNET session = WinHttpOpen(L"cswap-core/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { r.error = L"WinHttpOpen failed"; return r; }
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET req = conn ? WinHttpOpenRequest(conn, method.c_str(), path, nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              WINHTTP_FLAG_SECURE)
                         : nullptr;
    if (!req) {
        r.error = L"connect/open failed";
        if (conn) WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return r;
    }

    std::wstring headerBlock;
    for (const std::wstring& h : headers) headerBlock += h + L"\r\n";

    BOOL sent = WinHttpSendRequest(
        req, headerBlock.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlock.c_str(),
        headerBlock.empty() ? 0 : (DWORD)-1,
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(),
        (DWORD)body.size(), 0);
    if (sent && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = status;
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || !avail) break;
            std::string chunk(avail, 0);
            DWORD got = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &got)) break;
            chunk.resize(got);
            r.body += chunk;
        } while (avail);
        r.ok = true;
    } else {
        wchar_t msg[64];
        swprintf(msg, 64, L"winhttp error %lu", GetLastError());
        r.error = msg;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return r;
}

} // namespace cswap
