// Copyright (c) 2026 AydoganCan60- MIT License
#pragma once

#include <nvsdk_ngx.h>

NVSDK_NGX_Parameter* createNgxParameters(bool capabilities);
bool isEmulatedNgxParameters(const NVSDK_NGX_Parameter* parameters);
void destroyNgxParameters(NVSDK_NGX_Parameter* parameters);
