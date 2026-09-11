<!-- Copyright (c) 2026 AydoganCan60- MIT License -->
# dlssforamd

## Türkçe

`dlssforamd`, Windows/Wine altında `version.dll` olarak yüklenen deneysel bir D3D12 proxy'sidir. Microsoft Detours 4.0.1 ile yüklü NGX modülündeki D3D12 init/evaluate girişlerini yakalar ve NGX kaynak parametrelerini FidelityFX Upscaler API tanımlarına dönüştürmeyi dener.

### Önemli durum

Bu proje tamamlanmış, evrensel bir DLSS→FSR 3 çeviricisi değildir. NGX ABI'si oyun ve SDK sürümüne göre değişebilir. D3D12 DLSS capability, sentetik feature create/release ve evaluate yönlendirmesi deneyseldir. DLSS Frame Generation, tam parametre emülasyonu, kaynak durum geçişleri, descriptor yönetimi, UI ayrıştırma ve optik akış henüz uygulanmamıştır. Frame Generation bu nedenle destekleniyor olarak bildirilmez. Hatalı kaynak formatı/durumu GPU resetine neden olabilir. Çalışma zamanı olayları oyun dizinindeki `dlss_fsr_proxy.log` dosyasına yazılır.

### Bağımlılıklar

- MinGW-w64 x86_64 toolchain, CMake 3.21+
- Microsoft Detours **4.0.1** (`external/Detours`)
- AMD FidelityFX SDK (`external/FidelityFX-SDK`)
- Lisanslı NVIDIA NGX SDK başlıkları (`-DNGX_SDK_DIR=/yol/NGX`)
- Çalışma anında FidelityFX SDK ile gelen `amd_fidelityfx_upscaler_dx12.dll` (eski SDK için `amd_fidelityfx_dx12.dll`)

```bash
git clone --branch v4.0.1 https://github.com/microsoft/Detours.git external/Detours
git clone https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git external/FidelityFX-SDK
cmake -S . -B build -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
  -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
  -DNGX_SDK_DIR="$HOME/sdk/NGX"
cmake --build build --parallel
```

`version.dll` ve uygun FidelityFX runtime DLL'sini oyunun `.exe` dosyasının yanına kopyalayın. Steam başlatma seçeneği:

```text
WINEDLLOVERRIDES="version=n,b" %command%
```

Deneysel DirectCompute neural-engine yolunu etkinleştirmek için:

```text
DLSS_FOR_AMD_NEURAL=1 WINEDLLOVERRIDES="version=n,b" %command%
```

`shaders/matrix_mul.hlsl` dosyasını `version.dll` ile birlikte aynı dizin yapısında tutun. Motor shader'ı çalışma anında `d3dcompiler_47.dll` ile `cs_5_1` olarak derler ve sonucu `dlss_fsr_proxy.log` dosyasına yazar. Bu çekirdek açık model ağırlıkları bulunmadığından DLSS 5 ağını yeniden üretmez; WMMA kullanmayan tiled FP16/FP32 compute altyapısı ve G-buffer bağlantısı sağlar.

Proton logları ve oyun yedekleriyle test edin. Anti-cheat kullanan çevrimiçi oyunlarda DLL enjeksiyonu kuralları ihlal edebilir; bu projeyi kullanmayın.

## English

`dlssforamd` is an experimental D3D12 proxy loaded as `version.dll` on Windows/Wine. It uses Microsoft Detours 4.0.1 to intercept D3D12 NGX init/evaluate entry points and attempts to translate NGX resource parameters into FidelityFX Upscaler API descriptors.

### Important status

This is not a finished universal DLSS-to-FSR 3 translator. NGX ABIs vary across game/SDK versions. D3D12 DLSS capability overrides, synthetic feature create/release, and evaluate redirection are experimental. DLSS Frame Generation, full parameter emulation, resource transitions, descriptor management, UI separation, and optical flow are not implemented, so Frame Generation is not advertised as supported. Incorrect resource formats/states can trigger a GPU reset. Runtime events are written to `dlss_fsr_proxy.log` beside the game executable.

### Dependencies and build

Install CMake 3.21+ and the MinGW-w64 x86_64 toolchain. Clone Detours 4.0.1 and FidelityFX as shown in the Turkish section, obtain the NGX SDK headers under their applicable NVIDIA license, then run the same CMake commands. Copy `version.dll` and the matching FidelityFX runtime DLL beside the game executable.

Steam launch option:

```text
WINEDLLOVERRIDES="version=n,b" %command%
```

Experimental DirectCompute path:

```text
DLSS_FOR_AMD_NEURAL=1 WINEDLLOVERRIDES="version=n,b" %command%
```

Keep `shaders/matrix_mul.hlsl` beside the DLL using the same directory layout. It is compiled at runtime through `d3dcompiler_47.dll` as `cs_5_1`, with status written to `dlss_fsr_proxy.log`. This kernel does not reproduce the proprietary DLSS 5 network without open model weights; it provides a non-WMMA tiled FP16/FP32 compute foundation and G-buffer binding path.

Test with backups and Proton logging enabled. Do not use DLL injection with anti-cheat protected online games.

## License

Project-owned code is MIT licensed. Third-party dependencies and NGX headers retain their respective terms; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
