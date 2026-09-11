// Copyright (c) 2026 AydoganCan60- MIT License
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <detours.h>
#include <nvsdk_ngx.h>
#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>

namespace {
using NgxInit = NVSDK_NGX_Result (NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using NgxEvaluate = NVSDK_NGX_Result (NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using FfxCreate = ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using FfxDestroy = ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*);
using FfxDispatch = ffxReturnCode_t (*)(ffxContext*, const ffxDispatchDescHeader*);

NgxInit realInit = nullptr;
NgxEvaluate realEvaluate = nullptr;
FfxCreate ffxCreate = nullptr;
FfxDestroy ffxDestroy = nullptr;
FfxDispatch ffxDispatch = nullptr;
HMODULE fidelityFx = nullptr;
ffxContext upscaleContext{};
std::mutex stateMutex;
std::atomic_bool hooksInstalled{false};
std::atomic_bool contextReady{false};

FfxApiResource resource(ID3D12Resource* value) {
    FfxApiResource result{};
    result.resource = value;
    return result;
}

template <typename T>
bool getParameter(const NVSDK_NGX_Parameter* parameters, const char* name, T* output) {
    return parameters && parameters->Get(name, output) == NVSDK_NGX_Result_Success;
}

bool loadFidelityFx() {
    std::lock_guard<std::mutex> lock(stateMutex);
    if (fidelityFx) return true;
    fidelityFx = LoadLibraryW(L"amd_fidelityfx_upscaler_dx12.dll");
    if (!fidelityFx) fidelityFx = LoadLibraryW(L"amd_fidelityfx_dx12.dll");
    if (!fidelityFx) return false;
    ffxCreate = reinterpret_cast<FfxCreate>(GetProcAddress(fidelityFx, "ffxCreateContext"));
    ffxDestroy = reinterpret_cast<FfxDestroy>(GetProcAddress(fidelityFx, "ffxDestroyContext"));
    ffxDispatch = reinterpret_cast<FfxDispatch>(GetProcAddress(fidelityFx, "ffxDispatch"));
    return ffxCreate && ffxDestroy && ffxDispatch;
}

bool ensureContext(const NVSDK_NGX_Parameter* parameters) {
    if (contextReady.load()) return true;
    unsigned int renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
    if (!getParameter(parameters, "Width", &renderWidth) || !getParameter(parameters, "Height", &renderHeight)) return false;
    getParameter(parameters, "OutWidth", &outputWidth);
    getParameter(parameters, "OutHeight", &outputHeight);
    if (!outputWidth) outputWidth = renderWidth;
    if (!outputHeight) outputHeight = renderHeight;
    if (!loadFidelityFx()) return false;

    std::lock_guard<std::mutex> lock(stateMutex);
    if (contextReady.load()) return true;
    ffxCreateContextDescUpscale description{};
    description.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    description.maxRenderSize = {renderWidth, renderHeight};
    description.maxUpscaleSize = {outputWidth, outputHeight};
    description.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    contextReady.store(ffxCreate(&upscaleContext, &description.header, nullptr) == FFX_API_RETURN_OK);
    return contextReady.load();
}

NVSDK_NGX_Result NVSDK_CONV hookedInit(unsigned long long appId, const wchar_t* path, ID3D12Device* device,
                                       const NVSDK_NGX_FeatureCommonInfo* info, NVSDK_NGX_Version version) {
    if (loadFidelityFx()) return NVSDK_NGX_Result_Success;
    return realInit ? realInit(appId, path, device, info, version) : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
}

NVSDK_NGX_Result NVSDK_CONV hookedEvaluate(ID3D12GraphicsCommandList* commandList, const NVSDK_NGX_Handle* handle,
                                           const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
    ID3D12Resource *color = nullptr, *depth = nullptr, *motionVectors = nullptr, *output = nullptr;
    unsigned int width = 0, height = 0, outputWidth = 0, outputHeight = 0;
    float jitterX = 0.0f, jitterY = 0.0f, mvScaleX = 1.0f, mvScaleY = 1.0f, frameTime = 16.67f;
    const bool complete = getParameter(parameters, "Color", &color) && getParameter(parameters, "Depth", &depth) &&
        getParameter(parameters, "MotionVectors", &motionVectors) && getParameter(parameters, "Output", &output) &&
        getParameter(parameters, "Width", &width) && getParameter(parameters, "Height", &height);
    if (!complete || !ensureContext(parameters))
        return realEvaluate ? realEvaluate(commandList, handle, parameters, callback) : NVSDK_NGX_Result_FAIL_MissingInput;

    getParameter(parameters, "OutWidth", &outputWidth); getParameter(parameters, "OutHeight", &outputHeight);
    getParameter(parameters, "Jitter.Offset.X", &jitterX); getParameter(parameters, "Jitter.Offset.Y", &jitterY);
    getParameter(parameters, "MV.Scale.X", &mvScaleX); getParameter(parameters, "MV.Scale.Y", &mvScaleY);
    getParameter(parameters, "FrameTimeDeltaInMsec", &frameTime);

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = commandList;
    dispatch.color = resource(color); dispatch.depth = resource(depth); dispatch.motionVectors = resource(motionVectors); dispatch.output = resource(output);
    dispatch.jitterOffset = {jitterX, jitterY}; dispatch.motionVectorScale = {mvScaleX, mvScaleY};
    dispatch.renderSize = {width, height}; dispatch.upscaleSize = {outputWidth ? outputWidth : width, outputHeight ? outputHeight : height};
    dispatch.frameTimeDelta = frameTime; dispatch.preExposure = 1.0f; dispatch.cameraNear = 0.1f; dispatch.cameraFar = 1000.0f;
    return ffxDispatch(&upscaleContext, &dispatch.header) == FFX_API_RETURN_OK ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_PlatformError;
}

void hookNgx(HMODULE module) {
    if (!module || hooksInstalled.exchange(true)) return;
    realInit = reinterpret_cast<NgxInit>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init"));
    realEvaluate = reinterpret_cast<NgxEvaluate>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (!realInit || !realEvaluate) { hooksInstalled.store(false); return; }
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&realInit), hookedInit);
    DetourAttach(reinterpret_cast<PVOID*>(&realEvaluate), hookedEvaluate);
    if (DetourTransactionCommit() != NO_ERROR) hooksInstalled.store(false);
}

DWORD WINAPI initialize(LPVOID) {
    for (unsigned int attempt = 0; attempt < 600 && !hooksInstalled.load(); ++attempt) {
        HMODULE ngx = GetModuleHandleW(L"nvngx.dll");
        if (!ngx) ngx = GetModuleHandleW(L"nvngx_dlss.dll");
        if (ngx) hookNgx(ngx);
        Sleep(100);
    }
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
