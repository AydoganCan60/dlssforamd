// Copyright (c) 2026 AydoganCan60- MIT License
#include <windows.h>
#include "ngx_parameters.h"

#include <cstring>

namespace {
enum class ValueType { Empty, U64, Float, Double, UInt, Int, D3D11, D3D12, Pointer };

struct Entry {
    char name[128]{};
    ValueType type = ValueType::Empty;
    union Value {
        unsigned long long u64;
        float f32;
        double f64;
        unsigned int ui;
        int i;
        ID3D11Resource* d3d11;
        ID3D12Resource* d3d12;
        void* pointer;
        Value() : pointer(nullptr) {}
    } value;
};

class Parameters final : public NVSDK_NGX_Parameter {
public:
    void Set(const char* name, unsigned long long value) override { set(name, ValueType::U64).value.u64 = value; }
    void Set(const char* name, float value) override { set(name, ValueType::Float).value.f32 = value; }
    void Set(const char* name, double value) override { set(name, ValueType::Double).value.f64 = value; }
    void Set(const char* name, unsigned int value) override { set(name, ValueType::UInt).value.ui = value; }
    void Set(const char* name, int value) override { set(name, ValueType::Int).value.i = value; }
    void Set(const char* name, ID3D11Resource* value) override { set(name, ValueType::D3D11).value.d3d11 = value; }
    void Set(const char* name, ID3D12Resource* value) override { set(name, ValueType::D3D12).value.d3d12 = value; }
    void Set(const char* name, void* value) override { set(name, ValueType::Pointer).value.pointer = value; }

    NVSDK_NGX_Result Get(const char* name, unsigned long long* value) const override { return get(name, ValueType::U64, value, &Entry::Value::u64); }
    NVSDK_NGX_Result Get(const char* name, float* value) const override { return get(name, ValueType::Float, value, &Entry::Value::f32); }
    NVSDK_NGX_Result Get(const char* name, double* value) const override { return get(name, ValueType::Double, value, &Entry::Value::f64); }
    NVSDK_NGX_Result Get(const char* name, unsigned int* value) const override { return get(name, ValueType::UInt, value, &Entry::Value::ui); }
    NVSDK_NGX_Result Get(const char* name, int* value) const override { return get(name, ValueType::Int, value, &Entry::Value::i); }
    NVSDK_NGX_Result Get(const char* name, ID3D11Resource** value) const override { return get(name, ValueType::D3D11, value, &Entry::Value::d3d11); }
    NVSDK_NGX_Result Get(const char* name, ID3D12Resource** value) const override { return get(name, ValueType::D3D12, value, &Entry::Value::d3d12); }
    NVSDK_NGX_Result Get(const char* name, void** value) const override { return get(name, ValueType::Pointer, value, &Entry::Value::pointer); }

    void Reset() override {
        for (auto& entry : entries_) entry = {};
    }

private:
    Entry& set(const char* name, ValueType type) {
        Entry* entry = find(name);
        if (!entry) {
            for (auto& candidate : entries_) {
                if (candidate.type == ValueType::Empty) { entry = &candidate; break; }
            }
        }
        if (!entry) entry = &entries_[127];
        std::strncpy(entry->name, name ? name : "", sizeof(entry->name) - 1);
        entry->name[sizeof(entry->name) - 1] = '\0';
        entry->type = type;
        return *entry;
    }

    Entry* find(const char* name) {
        for (auto& entry : entries_)
            if (entry.type != ValueType::Empty && name && std::strcmp(entry.name, name) == 0) return &entry;
        return nullptr;
    }

    const Entry* find(const char* name) const {
        for (const auto& entry : entries_)
            if (entry.type != ValueType::Empty && name && std::strcmp(entry.name, name) == 0) return &entry;
        return nullptr;
    }

    template <typename T, typename Member>
    NVSDK_NGX_Result get(const char* name, ValueType type, T* output, Member member) const {
        const Entry* entry = find(name);
        if (!entry || entry->type != type || !output) return NVSDK_NGX_Result_FAIL_MissingInput;
        *output = entry->value.*member;
        return NVSDK_NGX_Result_Success;
    }

    Entry entries_[128]{};
};

Parameters* instances[64]{};
SRWLOCK instancesLock = SRWLOCK_INIT;
}

NVSDK_NGX_Parameter* createNgxParameters(bool capabilities) {
    auto* parameters = new Parameters();
    if (capabilities) {
        parameters->Set(NVSDK_NGX_Parameter_SuperSampling_Available, 1U);
        parameters->Set(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, static_cast<int>(NVSDK_NGX_Result_Success));
        parameters->Set("FrameGeneration.Available", 0U);
    }
    Parameters* result = nullptr;
    AcquireSRWLockExclusive(&instancesLock);
    for (auto& instance : instances) {
        if (!instance) { instance = parameters; result = parameters; break; }
    }
    ReleaseSRWLockExclusive(&instancesLock);
    if (!result) delete parameters;
    return result;
}

bool isEmulatedNgxParameters(const NVSDK_NGX_Parameter* parameters) {
    AcquireSRWLockShared(&instancesLock);
    bool found = false;
    for (auto* instance : instances) if (instance == parameters) { found = true; break; }
    ReleaseSRWLockShared(&instancesLock);
    return found;
}

void destroyNgxParameters(NVSDK_NGX_Parameter* parameters) {
    AcquireSRWLockExclusive(&instancesLock);
    Parameters* removed = nullptr;
    for (auto& instance : instances) if (instance == parameters) { removed = instance; instance = nullptr; break; }
    ReleaseSRWLockExclusive(&instancesLock);
    delete removed;
}
