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

1. recursively walks the selected SAF tree for archives, loose `gamedata` folders and
   the game's own `fsgame.ltx`;
2. detects the game (SoC / Clear Sky / Call of Pripyat) and mirrors every archive into
   app-private storage through descriptor-backed symlinks, preserving the layout;
3. emits (or reuses, with only writable paths redirected) an `fsgame.ltx` and mounts
   the real engine VFS/configuration graph;
4. initializes the real engine and game module;
5. creates the SDL surface and native Vulkan device;
6. runs the original startup sequencer, UI and main menu.

The Vulkan bootstrap uploads DDS UI textures, renders font alpha correctly, animates
`.seq` textures, decodes OGM/Theora frames on the CPU and updates live Vulkan textures.
Intro movies and the animated main-menu background use original game resources and timing.

## Known limitations

- The renderer is a bespoke forward Vulkan pass: no dynamic shadows, particles,
  sky/clouds, rain, lens flares, wallmarks or post-processing are rendered yet
  (their render services are still no-ops).
- Dynamic-model normals are not yet rotated to world space, so sun/hemisphere
  lighting on rotated characters/trees is approximate.
- Loose (unarchived) `gamedata` files are not mounted through SAF descriptors yet;
  only archives are mirrored. Loose overlays are the next discovery milestone.
- Encrypted/corrupted `*.db.ntp` archives (ransomware damage) are skipped, not
  recovered; replace them with clean copies.
- Textures accumulate across level loads (the UI texture cache is only cleared on
  full detach), which can grow memory on long sessions.
- Dynamic video upload uses synchronous staging and queue waits; persistent
  asynchronous uploads are planned.
- Android pause/resume and surface loss need broader device testing.
- ARMv7 builds are disabled until the ARM64 gameplay renderer stabilizes.

## Local archive fixture

After building a desktop baseline, create a freely redistributable archive fixture with:

```powershell
./misc/android/build-xrarchive-fixture.ps1
```

The ignored `build/android-fixtures/gamedata.db0` output is only for local SAF/VFS tests
and is never packaged into the APK.
