// Copyright (c) 2026 AydoganCan60- MIT License
#include "logger.h"

#include <cstdarg>
#include <cstdio>

namespace {
HANDLE logFile = INVALID_HANDLE_VALUE;
SRWLOCK logLock = SRWLOCK_INIT;
}

void logging::initialize(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length == MAX_PATH) return;
    wchar_t* separator = path + length;
    while (separator != path && separator[-1] != L'\\' && separator[-1] != L'/') --separator;
    lstrcpyW(separator, L"dlss_fsr_proxy.log");
    logFile = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    write("proxy attached process=%lu module=%p", GetCurrentProcessId(), module);
}

void logging::write(const char* format, ...) {
    if (logFile == INVALID_HANDLE_VALUE) return;
    char message[1024]{};
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const int prefix = std::snprintf(message, sizeof(message), "[%02u:%02u:%02u.%03u] [tid=%lu] ", time.wHour, time.wMinute,
                                     time.wSecond, time.wMilliseconds, GetCurrentThreadId());
    if (prefix < 0 || static_cast<size_t>(prefix) >= sizeof(message)) return;
    va_list arguments;
    va_start(arguments, format);
    const int body = std::vsnprintf(message + prefix, sizeof(message) - static_cast<size_t>(prefix), format, arguments);
    va_end(arguments);
    if (body < 0) return;
    const size_t used = static_cast<size_t>(prefix) + static_cast<size_t>(body);
    const size_t bounded = used < sizeof(message) - 2 ? used : sizeof(message) - 2;
    message[bounded] = '\r';
    message[bounded + 1] = '\n';
    AcquireSRWLockExclusive(&logLock);
    DWORD written = 0;
    WriteFile(logFile, message, static_cast<DWORD>(bounded + 2), &written, nullptr);
    FlushFileBuffers(logFile);
    ReleaseSRWLockExclusive(&logLock);
}
