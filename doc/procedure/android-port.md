# OpenXRay Android port

This document is the implementation contract for the Android port. It records decisions that must remain stable while the engine is being made buildable and runnable on Android.

## Product and data boundary

The Android application is an open-source engine port. It does not contain, download, redistribute, or generate proprietary S.T.A.L.K.E.R. game data.

The user must own a supported PC release and copy its data to the Android device. The application will let the user select that directory, validate the required files, retain access through Android's Storage Access Framework, and launch the engine against the selected data. No file copied from a user's commercial game installation may be committed to this repository or packaged into an APK/AAB. In particular, the app must never package database archives, levels, videos, sounds, or textures from the PC release. Engine-owned compatibility configuration already present upstream is a separate category and remains subject to the final release audit.

Development fixtures must be newly created, freely redistributable, or generated test data. A local proprietary-data path belongs in `local.properties` or another ignored local file.

## Pinned stage-1 toolchain

The canonical versions live in `misc/android/toolchain.properties`:

- JDK 17
- Android compile/target SDK 36 and minimum SDK 26
- Android Build Tools 36.0.0
- Android NDK r29 (`29.0.14206865`)
- CMake 4.1.2 from the Android SDK
- Gradle 8.13 and Android Gradle Plugin 8.13.2
- `arm64-v8a` as the primary ABI
- `armeabi-v7a` as a secondary compatibility ABI

ARM64 is the first runtime target. ARMv7 must keep compiling, but it does not block the first on-device engine milestone. The 32-bit ABI has a smaller address space and will need separate memory-budget testing.

## Local Windows setup

Install JDK 17, then run from the repository root:

```powershell
./misc/android/bootstrap-sdk.ps1 -AcceptLicenses
```

By default the SDK is installed into `.toolchains/android-sdk` next to the repository. An explicit location can be supplied with `-SdkRoot`. The script resumes the official Google command-line-tools download, verifies its size and SHA-1, installs exact package versions, and compiles a C++17 probe for both ARM ABIs.

To verify an existing installation without changing it:

```powershell
./misc/android/verify-toolchain.ps1 -SdkRoot E:/path/to/android-sdk
```

The CI workflow performs the same compiler probe on Linux, then builds and lints the host APK for both ABIs.

The stage-2 Gradle host lives in `android/`. It compiles SDL2 from the pinned `Externals/SDL2` submodule, builds a native library for both ARM ABIs, and presents a separate launcher that obtains persistent read access to a user-selected directory through the Storage Access Framework. The selected tree is passed to native code as a content URI; converting that URI into engine filesystem operations belongs to stage 3.

## Current implementation status

- Stage 1 is complete: the pinned Windows/CI toolchain, both NDK compiler probes, submodules and the upstream desktop `Release|x64` regression build pass.
- Stage 2 is complete on a physical Android 12.1 ARM64 device: the launcher starts, persists a selected SAF tree, enables the engine action and reaches a stable SDL native window without a crash. A black window is expected at this host milestone.
- Stage 3 is complete. `XR_PLATFORM_ANDROID` is distinct from desktop Linux, and the core, filesystem and read-only SAF bridge compile and run for `arm64-v8a` and `armeabi-v7a`.
- Before opening a window, native code runs data-independent CRC32, path-normalization, LZO round-trip and matrix tests. It then initializes the real core and locator against an OpenXRay-owned fixture under the app-private `files` directory and reads a marker through `FS.r_open`. This deeper lifecycle/VFS smoke path is validated on a physical Android 12 ARM64 device.
- A first read-only JNI/Storage Access Framework bridge now compiles for both ABIs. It can enumerate a relative directory below the retained tree and transfer ownership of a read-only file descriptor to native code. Seekable `gamedata.db*` descriptors are registered with `xrCore`; the locator duplicates them instead of reopening `/proc/self/fd`, which Android's app SELinux domain denies. The descriptor-backed archive path is physically validated both with the generated fixture and with a user-owned licensed ShoC installation: 12 root archives and 28,164 VFS entries were indexed, then `config/system.ltx` was mapped and decompressed through `FS.r_open`. No archive bytes are copied into app storage, the APK, or the repository.
- Physical testing also fixed Bionic/Android faults exposed by the deeper lifecycle: nullable `pw_gecos`, a returning desktop `SIGSEGV` handler that looped on Bionic, an SDL log callback whose pointer-sized stack allocation corrupted long log messages, an unsafe signed LZHUF output-size check, and a missing encrypted-buffer rollback before the POSIX ShoC Russian-key fallback.
- `misc/android/build-xrarchive-fixture.ps1` uses the upstream desktop `xrCompress` to generate a tiny valid `gamedata.db0` containing only the freely redistributable Android marker under `misc/android/fixtures/`. It lets the SAF/archive path be tested before any licensed game installation is copied.
- Stage 4 is complete. Both Android ABIs build pinned OpenAL Soft 1.25.2, Ogg/Vorbis/Theora, libjpeg-turbo, LZO, LuaJIT, luabind, xrLuaFix, xrScriptEngine, OPCODE, ODE, GameSpy and Dear ImGui. The physical ARM64 smoke suite validates audio context creation, media decoding primitives, Lua/JIT and binding execution, physics/collision queries, GameSpy MD5, ImGui versioning, the real ShoC archive set and `xrCore` lifecycle. OpenAL remains a replaceable shared `libopenal.so` to preserve the LGPL boundary.
- Stage 5 is complete as the renderer-bootstrap gate. SDL creates an Android Vulkan surface, selects a graphics/present device, and creates a swapchain, render pass, framebuffers and graphics pipeline. Reviewable GLSL is compiled to embedded SPIR-V by the pinned NDK. The Vulkan renderer now lives for the complete SDL window lifecycle, presents a continuously animated three-vertex frame, advances the core host-frame counter and persists a non-empty driver pipeline cache under app-private storage. A physical Android 12.1 Mali-G57 run remained stable for thousands of frames. This is intentionally identified as a persistent host frame, not yet an OpenXRay scene.
- The full static Android renderer graph (`xrCDB`, `xrSound`, `xrEngine`, `xrParticles`, shared renderer, R2 and `xrRender_GL`) now compiles with NDK r29 for both `arm64-v8a` and `armeabi-v7a`. It remains intentionally outside `libmain.so` until its desktop OpenGL entry points and runtime dependencies have Android-safe implementations.
- Zink is now an optional capability-gated path rather than the primary path for every device. The current Mali-G57 exposes Vulkan 1.1, geometry/tessellation and transform feedback, but lacks multiple requirements for Mesa's correct OpenGL 4.1 profile, including `logicOp`, `fillModeNonSolid`, `shaderClipDistance`, `dualSrcBlend` and `multiViewport`/16 viewports. The APK logs a conservative profile preflight and selects native Vulkan; it must not spoof a GL version. Mesa's Android EGL platform also depends on Android platform integration that cannot simply be linked as private system libraries by an ordinary application.
- Stage 6 is in progress. `xrAICore`, `xrUICore`, `xrGameSpy`, `xrPhysics` and the complete `xrGame` source graph now compile with NDK r29 for both ARM ABIs. Clang portability fixes break an include cycle in `CGraphEngine` and move a debug-only incomplete-type access out of a monster-state template. A probe compiled inside `xrGame` is linked into `libmain.so` and passes on the ARM64 device (`game_PlayerState` is 233 bytes), proving that the APK has a real game-module ABI edge.
- The licensed ShoC preflight now validates `config/system.ltx`, `scripts/_g.script` and `config/ui/ui_mm_main.xml`, then loads the recursive configuration graph with the engine's real `CInifile` parser. The reference installation passes with 1,245 `system.ltx` sections and 152 `game.ltx` sections. The selected SAF tree remains read-only and no configuration bytes are copied into the APK, repository or app-private cache.
- The next Stage 6 blocker is explicit rather than hidden: `xrGameModule::initialize` and `CGamePersistent` require live `GEnv.Render`, render-factory, UI-render and debug-render implementations. The desktop GL factory cannot run on the reference Mali device, so these interfaces must be backed by native Vulkan before the menu/level lifecycle is enabled. Loose `gamedata` overlays and `mods/*.xdb*` archive discovery also remain to be added after the base single-player archive path.

The private fixture is not a substitute for game files. It exists only to test OpenXRay's filesystem code without copying any proprietary data into the source tree or APK.

Android providers are allowed to return a pipe for exclusive read mode. X-Ray archives require random access, so the host rejects a descriptor unless `lseek` and positional reads succeed. A later compatibility fallback may copy only non-seekable providers into an app-private cache; it must never silently duplicate a full installation.

Before Android-specific changes are accepted, the upstream Windows baseline can be checked with:

```powershell
./misc/android/verify-desktop-baseline.ps1
```

The checked-in Visual Studio projects currently request the VS 2022 `v143` toolset. On a machine with only Visual Studio 2026, select the installed compiler without modifying all project files:

```powershell
./misc/android/verify-desktop-baseline.ps1 -PlatformToolset v145
```

The script explicitly enables restore for legacy `packages.config` dependencies; plain `msbuild /restore` does not restore them.

## Runtime architecture decisions

- The Android app is a thin lifecycle, permissions, input, and storage host. Engine code remains native C++.
- SDL is the first window, event, controller, audio-device, and lifecycle bridge.
- Android must have its own `XR_PLATFORM_ANDROID` branch. Treating Bionic as ordinary desktop Linux is not a supported shortcut.
- Native Vulkan is the primary renderer-bootstrap path. Desktop OpenGL through Mesa Zink is optional and may be selected only after the device passes the OpenGL 4.1 capability profile and the regular-APK Mesa/EGL path initializes successfully. A native GLES renderer remains a possible later compatibility track.
- Native crashes must be symbolizable per ABI; release artifacts must retain unstripped symbols separately from packaged libraries.
- The app must accept a user-selected data tree. Hard-coded shared-storage paths are forbidden because they break scoped storage and make device behavior vendor-dependent.
- Writable engine state, caches, logs and generated compatibility files belong under the app-private directory. The user-selected SAF tree is treated as read-only commercial input.

## Stage gates

1. **Toolchain foundation:** exact local/CI toolchain, submodules, desktop baseline, both ARM compiler probes.
2. **Android host shell:** Gradle application, SDL lifecycle, native shared library, logcat, empty window on a real `arm64-v8a` device.
3. **Core portability:** Android platform layer, filesystem abstraction over the selected data tree, threading/timing/logging/dynamic-loading fixes, engine core linked into the APK.
4. **Dependencies and scripting:** Android builds for LuaJIT/PUC Lua fallback, OpenAL Soft, Ogg/Vorbis/Theora, LZO, JPEG, physics and remaining third-party code.
5. **Renderer bootstrap:** native Vulkan capability checks and swapchain, capability-gated Zink experiment, embedded shader/pipeline/cache paths and a persistent renderer lifecycle driven by the core host-frame loop.
6. **Game execution:** native Vulkan implementations of the engine render interfaces, Shadow of Chernobyl resource/configuration compatibility, `xrGame` initialization, first engine-rendered frame, menu, level loading, new game, save/load, audio, input and scripted gameplay.
7. **Performance and ABI expansion:** thermal/memory profiling, ARMv7 constraints, device matrix, controls/UI scaling, packaging and upgrade tests.
8. **Release gate:** licensing review, attribution, clean-room asset audit, user-facing copy/import documentation, reproducible signed release process.

Stage 0 from the original roadmap is intentionally the final release gate. The no-proprietary-data boundary above is already enforced as an engineering constraint so early implementation cannot accidentally depend on redistributable game assets.
