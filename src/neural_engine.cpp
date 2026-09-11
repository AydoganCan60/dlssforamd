// Copyright (c) 2026 AydoganCan60- MIT License
#include "neural_engine.h"
#include "logger.h"

#include <d3dcompiler.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using CompileFromFile = HRESULT (WINAPI*)(LPCWSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

void safeRelease(IUnknown*& object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

bool siblingPath(HMODULE module, const wchar_t* relativePath, wchar_t (&path)[MAX_PATH]) {
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (!length || length == MAX_PATH) return false;
    wchar_t* separator = path + length;
    while (separator != path && separator[-1] != L'\\' && separator[-1] != L'/') --separator;
    if (static_cast<size_t>(separator - path) + static_cast<size_t>(lstrlenW(relativePath)) >= MAX_PATH) return false;
    return lstrcpyW(separator, relativePath) != nullptr;
}
}

NeuralEngine::~NeuralEngine() {
    release();
}

void NeuralEngine::release() {
    IUnknown* object = pipelineState_; safeRelease(object); pipelineState_ = nullptr;
    object = rootSignature_; safeRelease(object); rootSignature_ = nullptr;
    object = descriptorHeap_; safeRelease(object); descriptorHeap_ = nullptr;
    object = weights_; safeRelease(object); weights_ = nullptr;
    object = device_; safeRelease(object); device_ = nullptr;
}

bool NeuralEngine::loadWeights(HMODULE proxyModule) {
    wchar_t path[MAX_PATH]{};
    std::vector<float> values;
    if (siblingPath(proxyModule, L"models\\espcn_x3.weights", path)) {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            struct Header { char magic[4]; std::uint32_t version; std::uint32_t count; std::uint32_t tensors; } header{};
            DWORD read = 0;
            if (ReadFile(file, &header, sizeof(header), &read, nullptr) && read == sizeof(header) &&
                header.magic[0] == 'N' && header.magic[1] == 'W' && header.magic[2] == 'E' && header.magic[3] == 'I' && header.version == 1) {
                values.resize(header.count);
                const DWORD bytes = static_cast<DWORD>(values.size() * sizeof(float));
                if (!ReadFile(file, values.data(), bytes, &read, nullptr) || read != bytes) values.clear();
                else logging::write("NeuralEngine loaded ESPCN weights path=%ls floats=%u tensors=%u", path, header.count, header.tensors);
            }
            CloseHandle(file);
        }
    }
    if (values.empty()) {
        values.assign(64, 1.0f);
        logging::write("NeuralEngine model weights unavailable; using identity preview weights");
    }

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = values.size() * sizeof(float);
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                       nullptr, IID_PPV_ARGS(&weights_));
    void* mapped = nullptr;
    if (SUCCEEDED(result)) result = weights_->Map(0, nullptr, &mapped);
    if (FAILED(result)) return false;
    std::memcpy(mapped, values.data(), values.size() * sizeof(float));
    weights_->Unmap(0, nullptr);
    weightCount_ = static_cast<UINT>(values.size());
    return true;
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
    if (!compile || !siblingPath(proxyModule, L"shaders\\matrix_mul.hlsl", path)) {
        logging::write("NeuralEngine shader compiler unavailable compiler=%p", compiler);
        if (compiler) FreeLibrary(compiler);
        return false;
    }

    char tileSetting[3]{};
    GetEnvironmentVariableA("DLSS_FOR_AMD_TILE", tileSetting, sizeof(tileSetting));
    tileSize_ = lstrcmpA(tileSetting, "8") == 0 ? 8 : lstrcmpA(tileSetting, "32") == 0 ? 32 : 16;
    char tileValue[3]{};
    wsprintfA(tileValue, "%u", tileSize_);
    const D3D_SHADER_MACRO macros[] = {{"TILE_SIZE", tileValue}, {nullptr, nullptr}};
    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    logging::write("NeuralEngine compiling shader path=%ls entry=main target=cs_5_1 tile=%u", path, tileSize_);
    const HRESULT compileResult = compile(path, macros, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
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
    range.NumDescriptors = 4;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_DESCRIPTOR_RANGE outputRange{};
    outputRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    outputRange.NumDescriptors = 1;
    outputRange.BaseShaderRegister = 0;
    outputRange.OffsetInDescriptorsFromTableStart = 4;
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
    heap.NumDescriptors = 5;
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
    if (!loadWeights(proxyModule)) {
        logging::write("NeuralEngine weight upload failed");
        release();
        return false;
    }
    logging::write("NeuralEngine initialized rootSignature=%p pso=%p descriptorHeap=%p weights=%p count=%u tile=%u",
                   rootSignature_, pipelineState_, descriptorHeap_, weights_, weightCount_, tileSize_);
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
    D3D12_SHADER_RESOURCE_VIEW_DESC weightsView{};
    weightsView.Format = DXGI_FORMAT_UNKNOWN;
    weightsView.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    weightsView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    weightsView.Buffer.NumElements = weightCount_;
    weightsView.Buffer.StructureByteStride = sizeof(float);
    device_->CreateShaderResourceView(weights_, &weightsView, cpu);
    cpu.ptr += descriptorSize_;
    device_->CreateUnorderedAccessView(inputs.output, nullptr, nullptr, cpu);

    ID3D12DescriptorHeap* heaps[] = {descriptorHeap_};
    const UINT constants[] = {inputs.renderWidth, inputs.renderHeight, inputs.outputWidth, inputs.outputHeight};
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetComputeRootSignature(rootSignature_);
    commandList->SetPipelineState(pipelineState_);
    commandList->SetComputeRootDescriptorTable(0, descriptorHeap_->GetGPUDescriptorHandleForHeapStart());
    commandList->SetComputeRoot32BitConstants(1, 4, constants, 0);
    const UINT groupsX = (inputs.outputWidth + tileSize_ - 1) / tileSize_;
    const UINT groupsY = (inputs.outputHeight + tileSize_ - 1) / tileSize_;
    commandList->Dispatch(groupsX, groupsY, 1);
    logging::write("NeuralEngine dispatch recorded commandList=%p groups=%ux%u tile=%u weights=%u resources=(%p,%p,%p,%p)", commandList,
                   groupsX, groupsY, tileSize_, weightCount_, inputs.color, inputs.depth, inputs.motionVectors, inputs.output);
    return true;
}
