# XC2 Recomp — Android (`arm64-v8a`)

**XC2 Recomp (Android)** is a static recompilation of *Xenoblade Chronicles 2* (Nintendo Switch, 2017) into a native **Android `arm64-v8a`** application with a Vulkan 1.3 renderer, custom Mesa Turnip driver loading, unified memory management for 8 GB RAM handhelds, and 60 FPS support.

It is an Android port forked from [XC2 Recomp](https://github.com/AksharDP/xc2-recomp) (itself built on [XCX Recomp](https://github.com/jmars125/xcx-recomp) and [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) by Valiantsin Dzerakh), while keeping desktop Windows and Linux `x86-64` builds intact. Architecture notes for the Android port are in [docs/android-audit.md](docs/android-audit.md) and [docs/architecture.md](docs/architecture.md).

> [!IMPORTANT]
> **No game files, keys, or game-derived binaries are included.** You need your own legally obtained copy of *Xenoblade Chronicles 2* dumped from your own Nintendo Switch:
> - Base game (`0100E95004038000` Americas/Europe, or `0100F3400332C000` Japan)
> - Update **2.1.0** or **3.0.1** (`3.0.0` for Japan)
> - Optional add-on content (DLC) NSPs
> - Your console's `prod.keys`
>
> `libmain.so` and the final APK are compiled locally on your computer from your own game files and are strictly for personal use — **do not redistribute built APKs or `libmain.so`**.

---

## Android Features

- **Native ARM64 Recompilation (Not Emulation):** Guest Switch AArch64 code is statically lifted to C and compiled with Android NDK Clang (`-march=armv8.2-a+fp16+dotprod`) with native AAPCS64 fibers, hardware `fpcr` rounding/flush-to-zero sync, ARMv8 crypto `SHA-256`, NEON vector helpers, and `SA_ONSTACK` async virtual-core preemption.
- **8 GB Unified RAM Profile (`memory = unified_vulkan`):** Zero-copy unified memory allocation paired with a bounded 128 MB dirty staging ring and `mprotect`/`SIGSEGV` page write tracking, eliminating the desktop 1.3 GiB shadow pool copy so 8 GB handhelds stay well below Android `lmkd` limits.
- **Custom Mesa Turnip & Stock Adreno Support:** Dynamically loads bundled or custom Mesa Turnip drivers (`vk_driver = turnip`) via `libadrenotools` alongside stock Qualcomm Vulkan drivers.
- **On-Device NativeAOT Shader Translator:** Cross-compiles the Ryujinx Maxwell-to-SPIR-V shader translator with `.NET 10 NativeAOT` (`linux-bionic-arm64`) into `libxc2_xlate.so` with Android 16 KB page alignment and Scudo TBI compatibility.
- **60 FPS Real-Time Frame Step (`2.1.0` & `3.0.1`):** Runs up to 60 FPS with dynamic real-time frame stepping (frame dips do not cause slow-motion), scaled particle/sky/cloud-sea/camera/cloth steps, and automatic 30 FPS switching in cutscenes to preserve lip-sync (`recomp/patches-v210.txt`, `recomp/patches-v301.txt`).
- **Handheld Display & Input Integration:**
  - Native hardware ASTC textures (`astc = native`)
  - 720p internal target (`render_scale = 75` supported) with AMD FSR 1 EASU+RCAS upscaling to 1080p
  - Zero-copy hardware DPU display rotation (`VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR`) for portrait-native handheld AMOLED panels
  - Full SDL3 controller, rumble, and motion support, plus touch gestures (**3-finger tap** or Android **Back** toggles the settings menu; **4-finger tap** toggles the FPS/performance overlay)
  - Snapdragon big/Prime vs. efficiency core thread pinning (`android_pin = 1`)

---

## Target Hardware

Tested and tuned on **Qualcomm Snapdragon 8 Gen 2 / Adreno 740 / 8 GB LPDDR5X** handhelds (Retroid Pocket 6, AYN Odin 2, and equivalent Android 13+ devices):

- **OS:** Android 10+ (`arm64-v8a`, API 29+; Android 13+ recommended for custom Turnip drivers)
- **GPU:** Adreno 6xx / 7xx with Mesa Turnip (recommended) or Vulkan 1.3 driver supporting `shaderInt64`, `geometryShader`, `textureCompressionASTC_LDR`, `textureCompressionBC`, and `VK_EXT_depth_clip_control`
- **Storage:** ~19 GB free on device (`/sdcard/Android/data/com.xc2recomp.app/files/`) with base game + update + all 7 DLCs (~14.5 GB without DLCs)

---

## Building & Installing for Android

### 1. Host Prerequisites (macOS / Linux / Windows)

- **Android SDK & NDK** (NDK r26+ or r27+, platform `android-34`, `cmake`, `ninja`, `adb`) and **Gradle** (8.x/9.x with JDK 17+)
- **Python 3.12** (`.venv` with `numpy`, `lz4`, `cryptography`) for the recompiler and build scripts, and **Python 3.14** (`.venv-py314`) for NSP/NCA extraction
- **.NET 10 SDK** for cross-compiling the NativeAOT `libxc2_xlate.so` shader translator

### 2. Build Android Native Dependencies & Shader Translator

```bash
# Build SDL3, libopus, libvpx, zstd, and libadrenotools for arm64-v8a into toolchain/deps-android/
.venv/bin/python tools/setup_android_deps.py

# Cross-compile the .NET 10 NativeAOT shader translator (build/xlate/libxc2_xlate.so)
.venv/bin/python tools/build_xlate.py --target android-arm64
```

*(Optional)* Place a Mesa Turnip driver binary (`vulkan.adreno.so` / `vulkan.purple.so` extracted from a Turnip `.adpkg.zip`) at `toolchain/deps-android/lib/libvulkan_turnip.so` so `package_android.py` bundles it into the APK.

### 3. Extract Your Game NSPs

Configure `config.local.toml` (copied from [config.example.toml](config.example.toml)) or pass paths directly (`--version v210` for Update 2.1.0, or `--version v301` for Update 3.0.1):

```bash
.venv-py314/bin/python tools/xc2_extract.py \
  --base /path/to/base.nsp \
  --update /path/to/update.nsp \
  --dlc /path/to/dlc_folder \
  --keys /path/to/prod.keys \
  --out build/extracted/v210
```

### 4. Generate C Code & Compile `libmain.so`

For Update **2.1.0** (`v210`, use `recomp/patches-v301.txt` and `v301` for Update **3.0.1**):

```bash
.venv/bin/python recomp/gen.py \
  --exefs build/extracted/v210/exefs \
  --out generated/v210-android \
  --target android-arm64 \
  --patches recomp/patches-v210.txt

.venv/bin/python tools/build_game.py \
  --target android-arm64 \
  --version v210 \
  --gen generated/v210-android \
  --exefs build/extracted/v210/exefs \
  --romfs build/extracted/v210/romfs \
  --out build/v210-android
```

### 5. Stage Data & Build the APK

```bash
# Prepare build/xc2-data/ and stage JNI libraries into android/app/src/main/jniLibs/arm64-v8a/
.venv/bin/python tools/package_android.py --version v210

# Assemble the APK: android/app/build/outputs/apk/debug/app-debug.apk
gradle -p android assembleDebug
```

### 6. Install to Your Android Device

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell mkdir -p /sdcard/Android/data/com.xc2recomp.app/files
adb push -Z build/xc2-data/. /sdcard/Android/data/com.xc2recomp.app/files/
```

Launch **XC2 Recomp** on the device. Settings can be adjusted live in the in-game menu (**3-finger tap** or Android **Back**) or in `/sdcard/Android/data/com.xc2recomp.app/files/xc2.ini` (see [xc2.ini.example](xc2.ini.example)). Logs are written to `/sdcard/Android/data/com.xc2recomp.app/files/logs/xc2.log`.

---

## Desktop Builds (Windows & Linux `x86-64`)

Desktop `x86-64` builds remain supported; see [docs/building.md](docs/building.md) and [docs/installer.md](docs/installer.md). Before committing changes, always run:

```bash
python3 tools/check_content.py --staged
```

---

## Credits

XC2 Recomp builds on the projects below. Full attributions and licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):

- [XC2 Recomp](https://github.com/AksharDP/xc2-recomp), the upstream PC recompilation project
- [TotK Recomp](https://github.com/valentderah/totk-pc-recomp) by Valiantsin Dzerakh and [XCX Recomp](https://github.com/jmars125/xcx-recomp)
- [LAS Recomp](https://github.com/peiikai/LASRecomp)
- [MacNativeGames](https://github.com/ly05010419/MacNativeGames)
- [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) and [nxnative](https://github.com/seedds/nxnative)
- [SavageWindRecomp](https://github.com/Revivedx/SavageWindRecomp)
- [Ryujinx](https://github.com/Ryujinx/Ryujinx)
- [libadrenotools](https://github.com/bylaws/libadrenotools) and [liblinkernsbypass](https://github.com/bylaws/liblinkernsbypass) by Billy Laws
- [deko3d](https://github.com/devkitPro/deko3d) and [hactool](https://github.com/SciresM/hactool)
- [SDL 3](https://libsdl.org), [Dear ImGui](https://github.com/ocornut/imgui), [AMD FSR 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR), [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), [libopus](https://opus-codec.org), [libvpx](https://chromium.googlesource.com/webm/libvpx), [Zstandard](https://github.com/facebook/zstd)
- [Kenney](https://kenney.nl) Input Prompts (CC0) and the [Switchbrew](https://switchbrew.org) community

## License

The project's own code is licensed under the [MIT License](LICENSE); third-party components retain their respective licenses ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Nothing in this repository grants any rights to Nintendo's copyrighted game software or assets.

XC2 Recomp is an independent, non-commercial fan project and is not affiliated with or endorsed by Nintendo or Monolith Soft.
