# Android build and test guide

The Android project builds an engine-only development preview. No commercial game data
is embedded in the APK.

## Current target

- Android API 26+;
- `arm64-v8a` only for the current stabilization cycle;
- native Vulkan renderer;
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

Choose the root of a legally obtained Shadow of Chernobyl installation. The launcher
retains read permission through Android's Storage Access Framework and passes the URI to
native C++; it is never treated as an ordinary filesystem path.

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

1. validates the selected SAF tree;
2. exposes seekable `gamedata.db*` archives through descriptor-backed private links;
3. mounts and validates the ShoC VFS/configuration graph;
4. initializes the real engine and game module;
5. creates the SDL surface and native Vulkan device;
6. runs the original startup sequencer, UI and main menu.

The Vulkan bootstrap uploads DDS UI textures, renders font alpha correctly, animates
`.seq` textures, decodes OGM/Theora frames on the CPU and updates live Vulkan textures.
Intro movies and the animated main-menu background use original game resources and timing.

## Known limitations

- The world renderer is still a bootstrap: level geometry and models are not visible.
- Starting a new game reaches level/object initialization but not playable rendering.
- Some render-factory services remain no-op implementations required for staged startup.
- Dynamic video upload currently uses synchronous staging and queue waits; this will be
  replaced with persistent asynchronous uploads.
- Android pause/resume and surface loss need broader device testing.
- Touch-first controls are not implemented.

## Local archive fixture

After building a desktop baseline, create a freely redistributable archive fixture with:

```powershell
./misc/android/build-xrarchive-fixture.ps1
```

The ignored `build/android-fixtures/gamedata.db0` output is only for local SAF/VFS tests
and is never packaged into the APK.
