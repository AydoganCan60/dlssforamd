// Copyright (c) 2026 AydoganCan60- MIT License
#ifndef TILE_SIZE
#define TILE_SIZE 16
#endif

Texture2D<float4> InputColor : register(t0);
Texture2D<float> InputDepth : register(t1);
Texture2D<float2> InputMotion : register(t2);
StructuredBuffer<float> ModelWeights : register(t3);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer DispatchConstants : register(b0) {
    uint RenderWidth;
    uint RenderHeight;
    uint OutputWidth;
    uint OutputHeight;
};

groupshared float TileA[TILE_SIZE][TILE_SIZE];
groupshared float TileB[TILE_SIZE][TILE_SIZE];

[numthreads(TILE_SIZE, TILE_SIZE, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID) {
    float2 scale = float2(RenderWidth, RenderHeight) / float2(OutputWidth, OutputHeight);
    uint2 pixel = min(uint2(dispatchId.xy * scale), uint2(RenderWidth - 1, RenderHeight - 1));
    float4 color = InputColor.Load(int3(pixel, 0));
    float depth = InputDepth.Load(int3(pixel, 0));
    float2 motion = InputMotion.Load(int3(pixel, 0));

    TileA[localId.y][localId.x] = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    TileB[localId.y][localId.x] = saturate(1.0 - depth) + length(motion);
    GroupMemoryBarrierWithGroupSync();

    float fp32Accumulation = 0.0;
    min16float fp16Accumulation = min16float(0.0);
    [unroll]
    for (uint k = 0; k < TILE_SIZE; ++k) {
        float weight = ModelWeights[(localId.y * TILE_SIZE + k) % 64];
        float product = TileA[localId.y][k] * TileB[k][localId.x] * weight;
        fp32Accumulation += product;
        fp16Accumulation += min16float(product);
    }

    if (dispatchId.x < OutputWidth && dispatchId.y < OutputHeight) {
        float inferenceValue = 0.5 * (float(fp16Accumulation) + fp32Accumulation);
        float confidence = saturate(inferenceValue / TILE_SIZE);
        OutputColor[dispatchId.xy] = float4(lerp(color.rgb, confidence.xxx, 0.125), color.a);
    }
}
