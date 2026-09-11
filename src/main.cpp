// Copyright (c) 2026 AydoganCan60- MIT License
#include <windows.h>
#include <d3d12.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include <nvsdk_ngx.h>
#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>
#include "detours_mingw_compat.h"
#include "logger.h"
#include "neural_engine.h"
#include "ngx_parameters.h"
#include <detours.h>

struct NVSDK_NGX_Handle {
    std::uint32_t magic;
    NVSDK_NGX_Feature feature;
    bool active;
};

namespace {
using NgxInit = NVSDK_NGX_Result (NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using NgxCapabilities = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using NgxCreate = NVSDK_NGX_Result (NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using NgxEvaluate = NVSDK_NGX_Result (NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using NgxRelease = NVSDK_NGX_Result (NVSDK_CONV*)(NVSDK_NGX_Handle*);
using NgxShutdown = NVSDK_NGX_Result (NVSDK_CONV*)(ID3D12Device*);
using LoadLibraryAFunction = HMODULE (WINAPI*)(LPCSTR);
using LoadLibraryWFunction = HMODULE (WINAPI*)(LPCWSTR);
using GetProcAddressFunction = FARPROC (WINAPI*)(HMODULE, LPCSTR);
using SlIsFeatureSupported = std::int32_t (WINAPI*)(std::uint32_t, const void*);
using FfxCreate = ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using FfxDestroy = ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*);
using FfxDispatch = ffxReturnCode_t (*)(ffxContext*, const ffxDispatchDescHeader*);

constexpr std::uint32_t syntheticHandleMagic = 0x46535233;
NgxInit realInit = nullptr;
NgxCapabilities realGetCapabilities = nullptr;
NgxCreate realCreate = nullptr;
NgxEvaluate realEvaluate = nullptr;
NgxRelease realRelease = nullptr;
NgxShutdown realShutdown = nullptr;
LoadLibraryAFunction realLoadLibraryA = LoadLibraryA;
LoadLibraryWFunction realLoadLibraryW = LoadLibraryW;
GetProcAddressFunction realGetProcAddress = GetProcAddress;
SlIsFeatureSupported realSlIsFeatureSupported = nullptr;
FfxCreate ffxCreate = nullptr;
FfxDestroy ffxDestroy = nullptr;
FfxDispatch ffxDispatch = nullptr;
HMODULE fidelityFx = nullptr;
HMODULE proxyModule = nullptr;
ffxContext upscaleContext{};
NeuralEngine neuralEngine;
SRWLOCK stateLock = SRWLOCK_INIT;
std::atomic_bool hooksInstalled{false};
std::atomic_bool contextReady{false};
std::atomic_bool neuralEnabled{false};
NVSDK_NGX_Handle syntheticHandles[16]{};

class ScopedExclusiveLock {
public:
    explicit ScopedExclusiveLock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~ScopedExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ScopedExclusiveLock(const ScopedExclusiveLock&) = delete;
    ScopedExclusiveLock& operator=(const ScopedExclusiveLock&) = delete;

private:
    SRWLOCK& lock_;
};

FfxApiResource resource(ID3D12Resource* value) {
    FfxApiResource result{};
    result.resource = value;
    return result;
}

bool isSynthetic(const NVSDK_NGX_Handle* candidate) {
    for (const auto& handle : syntheticHandles)
        if (candidate == &handle) return handle.active && handle.magic == syntheticHandleMagic;
    return false;
}

template <typename T>
bool getParameter(const NVSDK_NGX_Parameter* parameters, const char* name, T* output) {
    return parameters && parameters->Get(name, output) == NVSDK_NGX_Result_Success;
}

bool loadFidelityFx() {
    ScopedExclusiveLock lock(stateLock);
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

    ScopedExclusiveLock lock(stateLock);
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
    const bool available = loadFidelityFx();
    char neuralSetting[2]{};
    const bool requested = GetEnvironmentVariableA("DLSS_FOR_AMD_NEURAL", neuralSetting, sizeof(neuralSetting)) == 1 && neuralSetting[0] == '1';
    neuralEnabled.store(requested && neuralEngine.initialize(device, proxyModule));
    const NVSDK_NGX_Result result = available || neuralEnabled.load() ? NVSDK_NGX_Result_Success :
        (realInit ? realInit(appId, path, device, info, version) : NVSDK_NGX_Result_FAIL_FeatureNotSupported);
    logging::write("NVSDK_NGX_D3D12_Init appId=%llu path=%p device=%p info=%p version=0x%x ffx=%u neuralRequested=%u neuralReady=%u result=0x%x",
                   appId, path, device, info, static_cast<unsigned int>(version), available, requested, neuralEnabled.load(),
                   static_cast<unsigned int>(result));
    return result;
}

NVSDK_NGX_Result NVSDK_CONV hookedGetCapabilities(NVSDK_NGX_Parameter** output) {
    NVSDK_NGX_Result original = realGetCapabilities ? realGetCapabilities(output) : NVSDK_NGX_Result_FAIL_NotInitialized;
    if (original != NVSDK_NGX_Result_Success && output) {
        *output = createNgxParameters(true);
        if (*output) original = NVSDK_NGX_Result_Success;
    }
    if (original == NVSDK_NGX_Result_Success && output && *output) {
        (*output)->Set(NVSDK_NGX_Parameter_SuperSampling_Available, 1U);
        (*output)->Set(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, static_cast<int>(NVSDK_NGX_Result_Success));
        (*output)->Set("FrameGeneration.Available", 0U);
        (*output)->Set("FrameGeneration.FeatureInitResult", static_cast<int>(NVSDK_NGX_Result_FAIL_FeatureNotSupported));
    }
    logging::write("NVSDK_NGX_D3D12_GetCapabilityParameters output=%p parameters=%p result=0x%x dlss=1 frameGeneration=0",
                   output, output ? *output : nullptr, static_cast<unsigned int>(original));
    return original;
}

NVSDK_NGX_Result NVSDK_CONV hookedCreate(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Feature feature,
                                         NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** output) {
    if (!output) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    if (feature != NVSDK_NGX_Feature_SuperSampling) {
        const NVSDK_NGX_Result result = realCreate ? realCreate(commandList, feature, parameters, output) : NVSDK_NGX_Result_FAIL_FeatureNotSupported;
        logging::write("NVSDK_NGX_D3D12_CreateFeature feature=%d commandList=%p parameters=%p result=0x%x passthrough=1",
                       static_cast<int>(feature), commandList, parameters, static_cast<unsigned int>(result));
        return result;
    }
    ScopedExclusiveLock lock(stateLock);
    for (auto& handle : syntheticHandles) {
        if (!handle.active) {
            handle = {syntheticHandleMagic, feature, true};
            *output = &handle;
            logging::write("NVSDK_NGX_D3D12_CreateFeature feature=%d commandList=%p parameters=%p handle=%p result=0x%x",
                           static_cast<int>(feature), commandList, parameters, *output, static_cast<unsigned int>(NVSDK_NGX_Result_Success));
            return NVSDK_NGX_Result_Success;
        }
    }
    logging::write("NVSDK_NGX_D3D12_CreateFeature feature=%d result=0x%x reason=handle_capacity", static_cast<int>(feature),
                   static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_OutOfDate));
    return NVSDK_NGX_Result_FAIL_OutOfDate;
}

NVSDK_NGX_Result NVSDK_CONV hookedEvaluate(ID3D12GraphicsCommandList* commandList, const NVSDK_NGX_Handle* handle,
                                           const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
    if (!isSynthetic(handle)) {
        const NVSDK_NGX_Result result = realEvaluate ? realEvaluate(commandList, handle, parameters, callback) : NVSDK_NGX_Result_FAIL_InvalidParameter;
        logging::write("NVSDK_NGX_D3D12_EvaluateFeature commandList=%p handle=%p parameters=%p callback=%p result=0x%x passthrough=1",
                       commandList, handle, parameters, reinterpret_cast<void*>(callback), static_cast<unsigned int>(result));
        return result;
    }
    ID3D12Resource *color = nullptr, *depth = nullptr, *motionVectors = nullptr, *output = nullptr;
    unsigned int width = 0, height = 0, outputWidth = 0, outputHeight = 0;
    float jitterX = 0.0f, jitterY = 0.0f, mvScaleX = 1.0f, mvScaleY = 1.0f, frameTime = 16.67f;
    const bool complete = getParameter(parameters, "Color", &color) && getParameter(parameters, "Depth", &depth) &&
        getParameter(parameters, "MotionVectors", &motionVectors) && getParameter(parameters, "Output", &output) &&
        getParameter(parameters, "Width", &width) && getParameter(parameters, "Height", &height);
    if (!complete || !ensureContext(parameters)) {
        logging::write("NVSDK_NGX_D3D12_EvaluateFeature commandList=%p handle=%p parameters=%p color=%p depth=%p motion=%p output=%p size=%ux%u result=0x%x",
                       commandList, handle, parameters, color, depth, motionVectors, output, width, height,
                       static_cast<unsigned int>(NVSDK_NGX_Result_FAIL_MissingInput));
        return NVSDK_NGX_Result_FAIL_MissingInput;
    }

    getParameter(parameters, "OutWidth", &outputWidth); getParameter(parameters, "OutHeight", &outputHeight);
    getParameter(parameters, "Jitter.Offset.X", &jitterX); getParameter(parameters, "Jitter.Offset.Y", &jitterY);
    getParameter(parameters, "MV.Scale.X", &mvScaleX); getParameter(parameters, "MV.Scale.Y", &mvScaleY);
    getParameter(parameters, "FrameTimeDeltaInMsec", &frameTime);
    if (!outputWidth) outputWidth = width;
    if (!outputHeight) outputHeight = height;

    if (neuralEnabled.load()) {
        const NeuralEngineInputs inputs{color, depth, motionVectors, output, width, height, outputWidth, outputHeight};
        if (neuralEngine.dispatch(commandList, inputs)) {
            logging::write("NVSDK_NGX_D3D12_EvaluateFeature routed=NeuralEngine handle=%p result=0x%x", handle,
                           static_cast<unsigned int>(NVSDK_NGX_Result_Success));
            return NVSDK_NGX_Result_Success;
        }
        logging::write("NVSDK_NGX_D3D12_EvaluateFeature NeuralEngine dispatch failed; falling back to FidelityFX");
    }

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = commandList;
    dispatch.color = resource(color); dispatch.depth = resource(depth); dispatch.motionVectors = resource(motionVectors); dispatch.output = resource(output);
    dispatch.jitterOffset = {jitterX, jitterY}; dispatch.motionVectorScale = {mvScaleX, mvScaleY};
    dispatch.renderSize = {width, height}; dispatch.upscaleSize = {outputWidth ? outputWidth : width, outputHeight ? outputHeight : height};
    dispatch.frameTimeDelta = frameTime; dispatch.preExposure = 1.0f; dispatch.cameraNear = 0.1f; dispatch.cameraFar = 1000.0f;
    const ffxReturnCode_t ffxResult = ffxDispatch(&upscaleContext, &dispatch.header);
    const NVSDK_NGX_Result result = ffxResult == FFX_API_RETURN_OK ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_PlatformError;
    logging::write("NVSDK_NGX_D3D12_EvaluateFeature commandList=%p handle=%p parameters=%p color=%p depth=%p motion=%p output=%p render=%ux%u outputSize=%ux%u jitter=(%.4f,%.4f) mvScale=(%.4f,%.4f) frameMs=%.3f ffxResult=%u result=0x%x",
                   commandList, handle, parameters, color, depth, motionVectors, output, width, height, dispatch.upscaleSize.width,
                   dispatch.upscaleSize.height, jitterX, jitterY, mvScaleX, mvScaleY, frameTime, ffxResult, static_cast<unsigned int>(result));
    return result;
}

NVSDK_NGX_Result NVSDK_CONV hookedRelease(NVSDK_NGX_Handle* handle) {
    ScopedExclusiveLock lock(stateLock);
    if (isSynthetic(handle)) {
        handle->active = false;
        logging::write("NVSDK_NGX_D3D12_ReleaseFeature handle=%p result=0x%x synthetic=1", handle,
                       static_cast<unsigned int>(NVSDK_NGX_Result_Success));
        return NVSDK_NGX_Result_Success;
    }
    const NVSDK_NGX_Result result = realRelease ? realRelease(handle) : NVSDK_NGX_Result_FAIL_InvalidParameter;
    logging::write("NVSDK_NGX_D3D12_ReleaseFeature handle=%p result=0x%x passthrough=1", handle, static_cast<unsigned int>(result));
    return result;
}

NVSDK_NGX_Result NVSDK_CONV hookedShutdown(ID3D12Device* device) {
    const NVSDK_NGX_Result result = realShutdown ? realShutdown(device) : NVSDK_NGX_Result_Success;
    logging::write("NVSDK_NGX_D3D12_Shutdown1 device=%p result=0x%x", device, static_cast<unsigned int>(result));
    return result;
}

NVSDK_NGX_Result NVSDK_CONV fallbackAllocateParameters(NVSDK_NGX_Parameter** output) {
    if (!output) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    *output = createNgxParameters(false);
    const NVSDK_NGX_Result result = *output ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_OutOfDate;
    logging::write("virtual NVSDK_NGX_D3D12_AllocateParameters output=%p parameters=%p result=0x%x", output, *output,
                   static_cast<unsigned int>(result));
    return result;
}

NVSDK_NGX_Result NVSDK_CONV fallbackDestroyParameters(NVSDK_NGX_Parameter* parameters) {
    if (!isEmulatedNgxParameters(parameters)) return NVSDK_NGX_Result_FAIL_InvalidParameter;
    destroyNgxParameters(parameters);
    logging::write("virtual NVSDK_NGX_D3D12_DestroyParameters parameters=%p result=0x%x", parameters,
                   static_cast<unsigned int>(NVSDK_NGX_Result_Success));
    return NVSDK_NGX_Result_Success;
}

bool requestedStreamlineSpoof() {
    char setting[2]{};
    return GetEnvironmentVariableA("DLSS_FOR_AMD_STREAMLINE_SPOOF", setting, sizeof(setting)) == 1 && setting[0] == '1';
}

std::int32_t WINAPI observedSlIsFeatureSupported(std::uint32_t feature, const void* adapterInfo) {
    constexpr std::uint32_t streamlineFeatureDlss = 0;
    if (feature == streamlineFeatureDlss && requestedStreamlineSpoof()) {
        logging::write("slIsFeatureSupported feature=%u adapterInfo=%p result=0 override=1", feature, adapterInfo);
        return 0;
    }
    const std::int32_t result = realSlIsFeatureSupported ? realSlIsFeatureSupported(feature, adapterInfo) : 32;
    logging::write("slIsFeatureSupported feature=%u adapterInfo=%p result=%d override=0", feature, adapterInfo, result);
    return result;
}

bool requestedVirtualization() {
    char setting[2]{};
    return GetEnvironmentVariableA("DLSS_FOR_AMD_NEURAL", setting, sizeof(setting)) == 1 && setting[0] == '1';
}

bool trackedModuleName(const char* path) {
    if (!path) return false;
    const char* name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    return _stricmp(name, "sl.interposer.dll") == 0 || _stricmp(name, "sl.common.dll") == 0 ||
        _stricmp(name, "sl.dlss.dll") == 0 || _stricmp(name, "nvngx.dll") == 0 || _stricmp(name, "nvngx_dlss.dll") == 0;
}

bool virtualModuleName(const char* path) {
    if (!path || !requestedVirtualization()) return false;
    const char* name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    return _stricmp(name, "nvngx.dll") == 0 || _stricmp(name, "nvngx_dlss.dll") == 0;
}

HMODULE retainProxyModule() {
    HMODULE retained = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(reinterpret_cast<const void*>(&retainProxyModule)), &retained);
    return retained;
}

HMODULE WINAPI hookedLoadLibraryA(LPCSTR path) {
    HMODULE module = realLoadLibraryA(path);
    if (!module && virtualModuleName(path)) module = retainProxyModule();
    if (trackedModuleName(path)) logging::write("LoadLibraryA path=%s result=%p virtual=%u", path, module, module == proxyModule);
    return module;
}

HMODULE WINAPI hookedLoadLibraryW(LPCWSTR path) {
    HMODULE module = realLoadLibraryW(path);
    char narrow[MAX_PATH]{};
    if (path) WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, MAX_PATH, nullptr, nullptr);
    if (!module && virtualModuleName(narrow)) module = retainProxyModule();
    if (trackedModuleName(narrow)) logging::write("LoadLibraryW path=%ls result=%p virtual=%u", path, module, module == proxyModule);
    return module;
}

FARPROC WINAPI hookedGetProcAddress(HMODULE module, LPCSTR name) {
    if (name && requestedVirtualization()) {
        FARPROC replacement = nullptr;
        if (module == proxyModule) {
            if (std::strcmp(name, "NVSDK_NGX_D3D12_Init") == 0) replacement = reinterpret_cast<FARPROC>(hookedInit);
            else if (std::strcmp(name, "NVSDK_NGX_D3D12_GetCapabilityParameters") == 0) replacement = reinterpret_cast<FARPROC>(hookedGetCapabilities);
            else if (std::strcmp(name, "NVSDK_NGX_D3D12_AllocateParameters") == 0) replacement = reinterpret_cast<FARPROC>(fallbackAllocateParameters);
            else if (std::strcmp(name, "NVSDK_NGX_D3D12_DestroyParameters") == 0) replacement = reinterpret_cast<FARPROC>(fallbackDestroyParameters);
            else if (std::strcmp(name, "NVSDK_NGX_D3D12_CreateFeature") == 0) replacement = reinterpret_cast<FARPROC>(hookedCreate);
            else if (std::strcmp(name, "NVSDK_NGX_D3D12_EvaluateFeature") == 0) replacement = reinterpret_cast<FARPROC>(hookedEvaluate);
            else if (std::strcmp(name, "NVSDK_NGX_D3D12_ReleaseFeature") == 0) replacement = reinterpret_cast<FARPROC>(hookedRelease);
            else if (std::strcmp(name, "NVSDK_NGX_D3D12_Shutdown1") == 0) replacement = reinterpret_cast<FARPROC>(hookedShutdown);
        }
        if (std::strcmp(name, "slIsFeatureSupported") == 0) {
            realSlIsFeatureSupported = reinterpret_cast<SlIsFeatureSupported>(realGetProcAddress(module, name));
            if (realSlIsFeatureSupported) replacement = reinterpret_cast<FARPROC>(observedSlIsFeatureSupported);
        }
        if (replacement) {
            logging::write("GetProcAddress module=%p name=%s replacement=%p", module, name, reinterpret_cast<void*>(replacement));
            return replacement;
        }
    }
    return realGetProcAddress(module, name);
}

bool installLoaderHooks() {
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&realLoadLibraryA), reinterpret_cast<PVOID>(hookedLoadLibraryA));
    DetourAttach(reinterpret_cast<PVOID*>(&realLoadLibraryW), reinterpret_cast<PVOID>(hookedLoadLibraryW));
    DetourAttach(reinterpret_cast<PVOID*>(&realGetProcAddress), reinterpret_cast<PVOID>(hookedGetProcAddress));
    const LONG result = DetourTransactionCommit();
    logging::write("loader hooks LoadLibraryA=%p LoadLibraryW=%p GetProcAddress=%p result=%ld", reinterpret_cast<void*>(realLoadLibraryA),
                   reinterpret_cast<void*>(realLoadLibraryW), reinterpret_cast<void*>(realGetProcAddress), result);
    return result == NO_ERROR;
}

void hookNgx(HMODULE module) {
    if (!module || hooksInstalled.exchange(true)) return;
    realInit = reinterpret_cast<NgxInit>(GetProcAddress(module, "NVSDK_NGX_D3D12_Init"));
    realGetCapabilities = reinterpret_cast<NgxCapabilities>(GetProcAddress(module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    realCreate = reinterpret_cast<NgxCreate>(GetProcAddress(module, "NVSDK_NGX_D3D12_CreateFeature"));
    realEvaluate = reinterpret_cast<NgxEvaluate>(GetProcAddress(module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    realRelease = reinterpret_cast<NgxRelease>(GetProcAddress(module, "NVSDK_NGX_D3D12_ReleaseFeature"));
    realShutdown = reinterpret_cast<NgxShutdown>(GetProcAddress(module, "NVSDK_NGX_D3D12_Shutdown1"));
    if (!realInit || !realEvaluate) {
        logging::write("NGX hook installation failed module=%p init=%p evaluate=%p", module, reinterpret_cast<void*>(realInit), reinterpret_cast<void*>(realEvaluate));
        hooksInstalled.store(false);
        return;
    }
    DetourTransactionBegin(); DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&realInit), reinterpret_cast<PVOID>(hookedInit));
    if (realGetCapabilities) DetourAttach(reinterpret_cast<PVOID*>(&realGetCapabilities), reinterpret_cast<PVOID>(hookedGetCapabilities));
    if (realCreate) DetourAttach(reinterpret_cast<PVOID*>(&realCreate), reinterpret_cast<PVOID>(hookedCreate));
    DetourAttach(reinterpret_cast<PVOID*>(&realEvaluate), reinterpret_cast<PVOID>(hookedEvaluate));
    if (realRelease) DetourAttach(reinterpret_cast<PVOID*>(&realRelease), reinterpret_cast<PVOID>(hookedRelease));
    if (realShutdown) DetourAttach(reinterpret_cast<PVOID*>(&realShutdown), reinterpret_cast<PVOID>(hookedShutdown));
    const LONG result = DetourTransactionCommit();
    hooksInstalled.store(result == NO_ERROR);
    logging::write("NGX hooks module=%p init=%p capabilities=%p create=%p evaluate=%p release=%p shutdown=%p result=%ld",
                   module, reinterpret_cast<void*>(realInit), reinterpret_cast<void*>(realGetCapabilities), reinterpret_cast<void*>(realCreate),
                   reinterpret_cast<void*>(realEvaluate), reinterpret_cast<void*>(realRelease), reinterpret_cast<void*>(realShutdown), result);
}

DWORD WINAPI initialize(LPVOID) {
    installLoaderHooks();
    logging::write("NGX discovery started virtualization=%u", requestedVirtualization());
    for (unsigned int attempt = 0; attempt < 600 && !hooksInstalled.load(); ++attempt) {
        HMODULE ngx = GetModuleHandleW(L"nvngx.dll");
        if (!ngx) ngx = GetModuleHandleW(L"nvngx_dlss.dll");
        if (ngx) hookNgx(ngx);
        Sleep(100);
    }
    if (!hooksInstalled.load()) logging::write("NGX discovery timed out");
    return 0;
}
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        proxyModule = instance;
        logging::initialize(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, initialize, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
