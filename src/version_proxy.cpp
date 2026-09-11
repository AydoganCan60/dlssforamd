// Copyright (c) 2026 AydoganCan60- MIT License
#include <windows.h>

namespace {
HMODULE realVersion() {
    static HMODULE module = [] {
        wchar_t path[MAX_PATH]{};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length + 13 >= MAX_PATH) return static_cast<HMODULE>(nullptr);
        lstrcatW(path, L"\\version.dll");
        return LoadLibraryW(path);
    }();
    return module;
}

template <typename Function>
Function resolve(const char* name) {
    const HMODULE module = realVersion();
    return module ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}
}

#define FORWARD(ret, call, name, args, invoke, failure) \
extern "C" __declspec(dllexport) ret call name args { \
    using Function = ret (call*) args; \
    const auto function = resolve<Function>(#name); \
    return function ? function invoke : failure; \
}

FORWARD(BOOL, WINAPI, GetFileVersionInfoA, (LPCSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d), FALSE)
FORWARD(BOOL, WINAPI, GetFileVersionInfoW, (LPCWSTR a, DWORD b, DWORD c, LPVOID d), (a,b,c,d), FALSE)
FORWARD(BOOL, WINAPI, GetFileVersionInfoByHandle, (DWORD a, HANDLE b, LPVOID* c, PDWORD d), (a,b,c,d), FALSE)
FORWARD(BOOL, WINAPI, GetFileVersionInfoExA, (DWORD a, LPCSTR b, DWORD c, DWORD d, LPVOID e), (a,b,c,d,e), FALSE)
FORWARD(BOOL, WINAPI, GetFileVersionInfoExW, (DWORD a, LPCWSTR b, DWORD c, DWORD d, LPVOID e), (a,b,c,d,e), FALSE)
FORWARD(DWORD, WINAPI, GetFileVersionInfoSizeA, (LPCSTR a, LPDWORD b), (a,b), 0)
FORWARD(DWORD, WINAPI, GetFileVersionInfoSizeW, (LPCWSTR a, LPDWORD b), (a,b), 0)
FORWARD(DWORD, WINAPI, GetFileVersionInfoSizeExA, (DWORD a, LPCSTR b, LPDWORD c), (a,b,c), 0)
FORWARD(DWORD, WINAPI, GetFileVersionInfoSizeExW, (DWORD a, LPCWSTR b, LPDWORD c), (a,b,c), 0)
FORWARD(DWORD, WINAPI, VerFindFileA, (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h), (a,b,c,d,e,f,g,h), 0)
FORWARD(DWORD, WINAPI, VerFindFileW, (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h), (a,b,c,d,e,f,g,h), 0)
FORWARD(DWORD, WINAPI, VerInstallFileA, (DWORD a, LPSTR b, LPSTR c, LPSTR d, LPSTR e, LPSTR f, LPSTR g, PUINT h), (a,b,c,d,e,f,g,h), 0)
FORWARD(DWORD, WINAPI, VerInstallFileW, (DWORD a, LPWSTR b, LPWSTR c, LPWSTR d, LPWSTR e, LPWSTR f, LPWSTR g, PUINT h), (a,b,c,d,e,f,g,h), 0)
FORWARD(DWORD, WINAPI, VerLanguageNameA, (DWORD a, LPSTR b, DWORD c), (a,b,c), 0)
FORWARD(DWORD, WINAPI, VerLanguageNameW, (DWORD a, LPWSTR b, DWORD c), (a,b,c), 0)
FORWARD(BOOL, WINAPI, VerQueryValueA, (LPCVOID a, LPCSTR b, LPVOID* c, PUINT d), (a,b,c,d), FALSE)
FORWARD(BOOL, WINAPI, VerQueryValueW, (LPCVOID a, LPCWSTR b, LPVOID* c, PUINT d), (a,b,c,d), FALSE)
