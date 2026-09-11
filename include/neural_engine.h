// Copyright (c) 2026 AydoganCan60- MIT License
#pragma once

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

struct NeuralEngineInputs {
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motionVectors = nullptr;
    ID3D12Resource* output = nullptr;
    std::uint32_t renderWidth = 0;
    std::uint32_t renderHeight = 0;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
};

class NeuralEngine {
public:
    NeuralEngine() = default;
    ~NeuralEngine();
    NeuralEngine(const NeuralEngine&) = delete;
    NeuralEngine& operator=(const NeuralEngine&) = delete;

    bool initialize(ID3D12Device* device, HMODULE proxyModule);
    bool dispatch(ID3D12GraphicsCommandList* commandList, const NeuralEngineInputs& inputs);
    bool ready() const { return pipelineState_ != nullptr; }

private:
    void release();

    ID3D12Device* device_ = nullptr;
    ID3D12RootSignature* rootSignature_ = nullptr;
    ID3D12PipelineState* pipelineState_ = nullptr;
    ID3D12DescriptorHeap* descriptorHeap_ = nullptr;
    UINT descriptorSize_ = 0;
};
