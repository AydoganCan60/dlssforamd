// Copyright (c) 2026 AydoganCan60- MIT License
#include "neural_engine.h"
#include "logger.h"

#include <d3dcompiler.h>

namespace {
using CompileFromFile = HRESULT (WINAPI*)(LPCWSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

void safeRelease(IUnknown*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

bool shaderPath(HMODULE module, wchar_t (&path)[MAX_PATH]) {
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length == MAX_PATH) return false;
    wchar_t* separator = path + length;
    while (separator != path && separator[-1] != L'\\' && separator[-1] != L'/') --separator;
    return lstrcpyW(separator, L"shaders\\matrix_mul.hlsl") != nullptr;
}
}

NeuralEngine::~NeuralEngine() {
    release();
}

void NeuralEngine::release() {
    IUnknown* object = pipelineState_; safeRelease(object); pipelineState_ = nullptr;
    object = rootSignature_; safeRelease(object); rootSignature_ = nullptr;
    object = descriptorHeap_; safeRelease(object); descriptorHeap_ = nullptr;
    object = device_; safeRelease(object); device_ = nullptr;
}

bool NeuralEngine::initialize(ID3D12Device* device, HMODULE proxyModule) {
    if (ready()) return true;
    if (!device || !proxyModule) {
        logging::write("NeuralEngine initialization rejected device=%p module=%p", device, proxyModule);
        return false;
    }

    logging::write("NeuralEngine initialization started device=%p", device);
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = compiler ? reinterpret_cast<CompileFromFile>(GetProcAddress(compiler, "D3DCompileFromFile")) : nullptr;
    wchar_t path[MAX_PATH]{};
    if (!compile || !shaderPath(proxyModule, path)) {
        logging::write("NeuralEngine shader compiler unavailable compiler=%p", compiler);
        if (compiler) FreeLibrary(compiler);
        return false;
    }

    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    logging::write("NeuralEngine compiling shader path=%ls entry=main target=cs_5_1", path);
    const HRESULT compileResult = compile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
                                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (FAILED(compileResult)) {
        logging::write("NeuralEngine shader compilation failed hr=0x%08lx message=%s", static_cast<unsigned long>(compileResult),
                       errors ? static_cast<const char*>(errors->GetBufferPointer()) : "none");
        if (errors) errors->Release();
        if (shader) shader->Release();
        FreeLibrary(compiler);
        return false;
    }
    if (errors) errors->Release();
    logging::write("NeuralEngine shader compiled bytes=%zu", shader->GetBufferSize());

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE outputRange{};
    outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors = 1;
    outputRange.BaseShaderRegister = 0;
    outputRange.OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER parameters[2]{};
    D3D12_DESCRIPTOR_RANGE ranges[] = {range, outputRange};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 2;
    parameters[0].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[1].Constants.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rootDescription{};
    rootDescription.NumParameters = 2;
    rootDescription.pParameters = parameters;
    ID3DBlob* serialized = nullptr;
    ID3DBlob* rootErrors = nullptr;
    HRESULT result = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &rootErrors);
    if (SUCCEEDED(result)) result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature_));
    if (serialized) serialized->Release();
    if (rootErrors) rootErrors->Release();

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = rootSignature_;
    pipeline.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    if (SUCCEEDED(result)) result = device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&pipelineState_));
    shader->Release();
    FreeLibrary(compiler);

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 4;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(result)) result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&descriptorHeap_));
    if (FAILED(result)) {
        logging::write("NeuralEngine PSO initialization failed hr=0x%08lx", static_cast<unsigned long>(result));
        release();
        return false;
    }

    device_ = device;
    device_->AddRef();
    descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    logging::write("NeuralEngine initialized rootSignature=%p pso=%p descriptorHeap=%p", rootSignature_, pipelineState_, descriptorHeap_);
    return true;
}

bool NeuralEngine::dispatch(ID3D12GraphicsCommandList* commandList, const NeuralEngineInputs& inputs) {
    if (!ready() || !commandList || !inputs.color || !inputs.depth || !inputs.motionVectors || !inputs.output || !inputs.renderWidth ||
        !inputs.renderHeight || !inputs.outputWidth || !inputs.outputHeight) {
        logging::write("NeuralEngine dispatch rejected ready=%u commandList=%p color=%p depth=%p motion=%p output=%p render=%ux%u outputSize=%ux%u",
                       ready(), commandList, inputs.color, inputs.depth, inputs.motionVectors, inputs.output, inputs.renderWidth,
                       inputs.renderHeight, inputs.outputWidth, inputs.outputHeight);
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateShaderResourceView(inputs.color, nullptr, cpu);
    cpu.ptr += descriptorSize_;
    device_->CreateShaderResourceView(inputs.depth, nullptr, cpu);
    cpu.ptr += descriptorSize_;
    device_->CreateShaderResourceView(inputs.motionVectors, nullptr, cpu);
    cpu.ptr += descriptorSize_;
    device_->CreateUnorderedAccessView(inputs.output, nullptr, nullptr, cpu);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap_};
    const UINT constants[] = {inputs.renderWidth, inputs.renderHeight, inputs.outputWidth, inputs.outputHeight};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(rootSignature_);
    commandList->SetPipelineState(pipelineState_);
    commandList->SetComputeRootDescriptorTable(0, descriptorHeap_->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRoot32BitConstants(1, 4, constants, 0);
    commandList->Dispatch((inputs.outputWidth + 7) / 8, (inputs.outputHeight + 7) / 8, 1);
    logging::write("NeuralEngine dispatch recorded commandList=%p groups=%ux%u resources=(%p,%p,%p,%p)", commandList,
                   (inputs.outputWidth + 7) / 8, (inputs.outputHeight + 7) / 8, inputs.color, inputs.depth, inputs.motionVectors, inputs.output);
    return true;
}
