# Android build and test guide

The Android project builds an engine-only development preview. No commercial game data
is embedded in the APK.

## Current target

- Android API 26+;
- `arm64-v8a` only for the current stabilization cycle;
- native Vulkan renderer (default) and the upstream X-Ray GL renderer on OpenGL ES 3.2
  (experimental, per-profile "Рендер" setting);
- Shadow of Chernobyl, Clear Sky, Call of Pripyat and mods built on them;
- pinned JDK 17, Gradle 8.13, Android SDK 36, NDK r29 and CMake 4.1.2.

ARMv7 source support is retained but its build is temporarily disabled until the ARM64
gameplay renderer is functional.

## Fresh checkout

Initialize the repository and submodules:

```powershell
git submodule update --init --recursive
```

The port carries reproducible compatibility patches for upstream submodules. Apply them
directly, or let the SDK bootstrap do it automatically:

```powershell
./misc/android/apply-submodule-patches.ps1
./misc/android/bootstrap-sdk.ps1 -AcceptLicenses
```

The patch command is idempotent: already-applied patches are detected and left alone.

## Build

From the repository root:

```powershell
$env:ANDROID_SDK_ROOT = (Resolve-Path ../.toolchains/android-sdk)
./android/gradlew.bat -p android assembleDebug
```

The APK is written to `android/app/build/outputs/apk/debug/app-debug.apk`.

Debug builds compile the engine at `-O2` with assertions enabled. For a faster,
non-debuggable build that keeps native symbols separately:

```powershell
./android/gradlew.bat -p android assembleRelease
```

The release APK is signed with the debug key so it can be sideloaded; unstripped symbols
for symbolizing tombstones are in `android/app/build/outputs/native-debug-symbols/`.

Before changing GL shaders, check them against both GLSL ES 3.20 and desktop GLSL 4.10:

```powershell
python misc/android/validate-gles-shaders.py
python misc/android/validate-gles-shaders.py --desktop
```

Run Android Lint with:

```powershell
./android/gradlew.bat -p android lintDebug
```

The lint baseline only suppresses findings inherited from pinned SDL2. New launcher or
host findings still fail the task.

## Install and run

```powershell
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n org.openxray.android/.LauncherActivity
```

Choose the directory of a legally obtained installation (or any folder that contains one).
The game is detected automatically; a profile can force Shadow of Chernobyl, Clear Sky,
Call of Pripyat or the unlocked mode for mixed mods.

Two access modes exist:

- **Direct** (recommended): grant "Разрешить прямой доступ к файлам" (all-files access).
  The engine then reads the installation through ordinary paths: archives, loose
  `gamedata` files and mods at native speed.
- **SAF** (no extra permission): the launcher keeps read permission through Android's
  Storage Access Framework; every file is exposed to the engine as a read-only virtual
  file, nothing is copied.

For logs:

```powershell
adb logcat -s OpenXRay
```

## Runtime architecture

The APK contains the SDL activity, launcher, SAF bridge and one native engine library.
The native graph includes `xrCore`, Lua/LuaJIT compatibility code, scripting, physics,
OpenAL Soft/OpenSL ES, Vorbis, Theora, image codecs, `xrEngine`, `xrGame` and the Android
Vulkan bridge.

At startup the host:

1. walks the selected tree once (directly or through SAF) and finds the installation
   root from its `fsgame.ltx`, `gamedata` folder or archive folders;
2. reuses the game's own `fsgame.ltx` with only writable paths (logs, saves,
   screenshots) redirected to app storage, or synthesizes one;
3. exposes the installation to the engine locator (a `gameroot` link in direct mode,
   virtual files in SAF mode) together with the packaged OpenXRay GL shaders;
4. mounts the real engine VFS, refines the game mode from the shipped levels and checks
   the configuration graph;
5. initializes the real engine and game module with the selected renderer;
6. runs the original startup sequencer, UI and main menu.

Going to the background pauses audio; returning rebuilds only the Vulkan swapchain, so
textures and meshes stay on the GPU.

The Vulkan bootstrap uploads DDS UI textures, renders font alpha correctly, animates
`.seq` textures, decodes OGM/Theora frames on the CPU and updates live Vulkan textures.
Intro movies and the animated main-menu background use original game resources and timing.

## Known limitations

- The renderer is a bespoke forward Vulkan pass: no dynamic shadows, particles,
  sky/clouds, rain, lens flares, wallmarks or post-processing are rendered yet
  (their render services are still no-ops).
- The OpenGL ES renderer is untested on devices: its shaders pass offline GLSL ES
  validation, but driver behaviour, performance and memory (BCn textures are decoded to
  RGBA8 on GPUs without S3TC; BC6H/BC7 have no decoder yet) still need a device matrix.
- Encrypted/corrupted `*.db.ntp` archives (ransomware damage) are skipped, not
  recovered; replace them with clean copies.
- Textures accumulate across level loads (the UI texture cache is only cleared on
  full detach), which can grow memory on long sessions.
- Dynamic video upload uses synchronous staging and queue waits; persistent
  asynchronous uploads are planned.
- Pause/resume and rotation are handled (swapchain-only rebuild, audio pause) but need
  broader device testing.
- ARMv7 builds are disabled until the ARM64 gameplay renderer stabilizes.

## Local archive fixture

After building a desktop baseline, create a freely redistributable archive fixture with:

```powershell
./misc/android/build-xrarchive-fixture.ps1
```

The ignored `build/android-fixtures/gamedata.db0` output is only for local SAF/VFS tests
and is never packaged into the APK.
