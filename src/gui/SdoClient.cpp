#include "SdoClient.h"
#include <windows.h>
#include <winhttp.h>
#include <cassert>
#include <format>

namespace TotalControl {

std::vector<uint8_t> FetchSdoJpeg(
    const std::function<void(std::string_view)>& logger)
{
    constexpr LPCWSTR kHost = L"sdo.gsfc.nasa.gov";
    constexpr LPCWSTR kPath = L"/assets/img/latest/latest_1024_HMIIC.jpg";
    constexpr DWORD   kTimeoutMs = 20000;

    HINTERNET hSes = WinHttpOpen(L"TotalControlSDO/1.0",
                                  WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                  WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) {
        if (logger) logger(std::format("SDO: WinHttpOpen failed ({})", GetLastError()));
        return {};
    }
    WinHttpSetTimeouts(hSes, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    HINTERNET hCon = WinHttpConnect(hSes, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hCon) {
        if (logger) logger(std::format("SDO: WinHttpConnect failed ({})", GetLastError()));
        WinHttpCloseHandle(hSes);
        return {};
    }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", kPath,
                                         nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE);
    if (!hReq) {
        if (logger) logger(std::format("SDO: WinHttpOpenRequest failed ({})", GetLastError()));
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    bool ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
           && WinHttpReceiveResponse(hReq, nullptr);
    if (!ok) {
        if (logger) logger(std::format("SDO: request failed ({})", GetLastError()));
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    // Check HTTP status
    DWORD status = 0;
    DWORD statusSz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        if (logger) logger(std::format("SDO: HTTP status {}", status));
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hCon);
        WinHttpCloseHandle(hSes);
        return {};
    }

    std::vector<uint8_t> body;
    body.reserve(512 * 1024);
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        size_t off = body.size();
        body.resize(off + avail);
        DWORD got = 0;
        if (!WinHttpReadData(hReq, body.data() + off, avail, &got)) break;
        body.resize(off + got);
        if (body.size() > 4 * 1024 * 1024) break;  // 4 MB safety cap
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hCon);
    WinHttpCloseHandle(hSes);

    if (body.empty())
        if (logger) logger("SDO: empty response");

    return body;
}

} // namespace TotalControl
