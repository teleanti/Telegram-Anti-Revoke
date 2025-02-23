﻿#include "Utils.h"

#include <wininet.h>

#include <memory>
#include <algorithm>

#include <spdlog/spdlog.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "Version.lib")

namespace File {

std::string GetCurrentFullNameA()
{
    char Buffer[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, Buffer, MAX_PATH) == 0) {
        return "";
    }

    return std::string{Buffer};
}

std::string GetCurrentName()
{
    std::string FullName = File::GetCurrentFullNameA();
    if (FullName.empty()) {
        return "";
    }

    size_t Position = FullName.rfind('\\');
    if (Position == std::string::npos) {
        return "";
    }

    Position++;
    return FullName.substr(Position, FullName.size() - Position);
}

uint32_t GetCurrentVersion()
{
    std::string FullName = GetCurrentFullNameA();

    ULONG InfoSize = GetFileVersionInfoSizeA(FullName.c_str(), nullptr);
    if (InfoSize == 0) {
        return 0;
    }

    std::unique_ptr<char[]> Buffer(new char[InfoSize]);
    if (!GetFileVersionInfoA(FullName.c_str(), 0, InfoSize, Buffer.get())) {
        return 0;
    }

    VS_FIXEDFILEINFO *pVsInfo = nullptr;
    UINT VsInfoSize = sizeof(VS_FIXEDFILEINFO);
    if (!VerQueryValueA(Buffer.get(), "\\", (PVOID *)&pVsInfo, &VsInfoSize)) {
        return 0;
    }

    return std::stoul(Text::Format(
        "%03hu%03hu%03hu", HIWORD(pVsInfo->dwFileVersionMS), LOWORD(pVsInfo->dwFileVersionMS),
        HIWORD(pVsInfo->dwFileVersionLS)));
}

} // namespace File

namespace Text {

std::string ToLower(const std::string &String)
{
    std::string Result = String;
    std::transform(Result.begin(), Result.end(), Result.begin(), tolower);
    return Result;
}

std::string SubReplace(const std::string &Source, const std::string &Target, const std::string &New)
{
    std::string Result = Source;
    while (true) {
        SIZE_T Pos = Result.find(Target);
        if (Pos == std::string::npos) {
            break;
        }
        Result.replace(Pos, Target.size(), New);
    }
    return Result;
}

std::vector<std::string> SplitByFlag(const std::string &Source, const std::string &Flag)
{
    std::vector<std::string> Result;
    SIZE_T BeginPos = 0, EndPos = Source.find(Flag);

    while (EndPos != std::string::npos) {
        Result.emplace_back(Source.substr(BeginPos, EndPos - BeginPos));

        BeginPos = EndPos + Flag.size();
        EndPos = Source.find(Flag, BeginPos);
    }

    if (BeginPos != Source.length()) {
        Result.emplace_back(Source.substr(BeginPos));
    }

    return Result;
}

std::string Format(const char *Format, ...)
{
    va_list VaList;
    char Buffer[0x200] = {0};

    va_start(VaList, Format);
    vsprintf_s(Buffer, Format, VaList);
    va_end(VaList);

    return std::string(Buffer);
}

} // namespace Text

namespace Convert {

std::string UnicodeToAnsi(const std::wstring &String)
{
    std::string Result;
    int Length = WideCharToMultiByte(
        CP_ACP, 0, String.c_str(), (int)String.length(), nullptr, 0, nullptr, nullptr);
    Result.resize(Length);
    WideCharToMultiByte(
        CP_ACP, 0, String.c_str(), (int)String.length(), (char *)Result.data(), Length, nullptr,
        nullptr);
    return Result;
}

} // namespace Convert

namespace Internet {

bool HttpRequest(
    std::string &Response, uint32_t &Status, const std::string &HttpVerb,
    const std::string &HostName, const std::string &ObjectName,
    const std::vector<std::pair<std::string, std::string>> &Headers, const std::string &PostData)
{
    if (HttpVerb != "GET" && HttpVerb != "POST") {
        return false;
    }

    if (HttpVerb != "POST" && !PostData.empty()) {
        return false;
    }

    bool Result = false;
    HINTERNET hInternet = nullptr, hConnect = nullptr, hRequest = nullptr;

    do {
        hInternet = InternetOpenA(
            "Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.0)", INTERNET_OPEN_TYPE_PRECONFIG,
            nullptr, nullptr, 0);
        if (hInternet == nullptr) {
            break;
        }

        // Set timeout values
        //
        ULONG Timeout = 30000;
        if (!InternetSetOptionA(
                hInternet, INTERNET_OPTION_CONNECT_TIMEOUT, &Timeout, sizeof(Timeout)) ||
            !InternetSetOptionA(
                hInternet, INTERNET_OPTION_RECEIVE_TIMEOUT, &Timeout, sizeof(Timeout)) ||
            !InternetSetOptionA(hInternet, INTERNET_OPTION_SEND_TIMEOUT, &Timeout, sizeof(Timeout)))
        {
            spdlog::critical(
                "InternetSetOptionA Set timeout failed. Last error: {}", ::GetLastError());
            break;
        }

        hConnect = InternetConnectA(
            hInternet, HostName.c_str(), INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr,
            INTERNET_SERVICE_HTTP, 0, 0);
        if (hConnect == nullptr) {
            break;
        }

        hRequest = HttpOpenRequestA(
            hConnect, HttpVerb.c_str(), ObjectName.c_str(), "HTTP/1.1", nullptr, nullptr,
            INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE, 0);
        if (hRequest == nullptr) {
            break;
        }

        std::string HeadersText;
        for (const auto &[HeaderName, HeaderValue] : Headers) {
            HeadersText += HeaderName + ": " + HeaderValue + "\r\n";
        }

        if (!HttpSendRequestA(
                hRequest, HeadersText.c_str(), -1, (void *)PostData.c_str(),
                (ULONG)PostData.size()))
        {
            break;
        }

        ULONG StatusSize = sizeof(Status), Index = 0;
        if (!HttpQueryInfoA(
                hRequest, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &Status, &StatusSize,
                &Index))
        {
            break;
        }

        Response.clear();

        while (true) {
#define ONCE_READ_SIZE (0x100)
            ULONG BytesRead = 0;
            char TempBuffer[ONCE_READ_SIZE + 1];
            memset(TempBuffer, 0, sizeof(TempBuffer));

            if (!InternetReadFile(hRequest, TempBuffer, ONCE_READ_SIZE, &BytesRead)) {
                goto EXIT;
            }

            if (BytesRead == 0) {
                break;
            }

            // We must fill a zero ending here.
            // Because the `InternetReadFile` API will write more data than `BytesRead` if the
            // buffer is large enough!
            TempBuffer[BytesRead] = '\0';

            Response += TempBuffer;
#undef ONCE_READ_SIZE
        }

        Result = true;

    } while (false);

EXIT:
    if (hRequest != nullptr) {
        InternetCloseHandle(hRequest);
    }
    if (hConnect != nullptr) {
        InternetCloseHandle(hConnect);
    }
    if (hInternet != nullptr) {
        InternetCloseHandle(hInternet);
    }

    return Result;
}

} // namespace Internet

namespace Memory {

void ReadProcess(HANDLE hProcess, void *TargetAddress, void *LocalBuffer, size_t Size)
{
    SIZE_T Bytes;
    ReadProcessMemory(hProcess, TargetAddress, LocalBuffer, Size, &Bytes);
}

bool ForceOperate(void *Address, size_t Size, const std::function<void()> &FnCallback)
{
    ULONG SaveProtect;

    if (!VirtualProtect(Address, Size, PAGE_EXECUTE_READWRITE, &SaveProtect)) {
        return FALSE;
    }

    FnCallback();

    return VirtualProtect(Address, Size, SaveProtect, &SaveProtect);
}

std::vector<uint8_t> MakeCall(void *HookAddress, void *CallAddress)
{
    std::vector<uint8_t> MakeCode = {0xE8, 0x00, 0x00, 0x00, 0x00};
    *(uint32_t *)(MakeCode.data() + 1) =
        (uint32_t)((uintptr_t)CallAddress - (uintptr_t)HookAddress - 5);
    return MakeCode;
}

std::vector<uint8_t> MakeJmp(void *HookAddress, void *JmpAddress)
{
    std::vector<uint8_t> MakeCode = {0xE9, 0x00, 0x00, 0x00, 0x00};
    *(uint32_t *)(MakeCode.data() + 1) =
        (uint32_t)((uintptr_t)JmpAddress - (uintptr_t)HookAddress - 5);
    return MakeCode;
}

} // namespace Memory

namespace Utils {

void CreateConsole()
{
    FILE *hStream = nullptr;
    AllocConsole();
    freopen_s(&hStream, "CONOUT$", "w", stdout);
}

} // namespace Utils