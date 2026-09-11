// Copyright (c) 2026 AydoganCan60- MIT License
#pragma once

#include <windows.h>

namespace logging {
void initialize(HMODULE module);
void write(const char* format, ...);
}
