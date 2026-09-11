// Copyright (c) 2026 AydoganCan60- MIT License
#include <windows.h>

extern "C" ULONG WINAPI DetourGetModuleSize(HMODULE module) {
    if (!module) return 0;
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(module) + dosHeader->e_lfanew);
    return ntHeaders->Signature == IMAGE_NT_SIGNATURE ? ntHeaders->OptionalHeader.SizeOfImage : 0;
}
