<div align="center">
  <img src="misc/media/OpenXraycover.png" alt="OpenXRay" width="760" />

  # OpenXRdroid16 — Android Port of OpenXray16
 
  **Experimental native Android port of S.T.A.L.K.E.R. Game**

  [![Android](https://img.shields.io/badge/Android-8.0%2B-3DDC84?logo=android&logoColor=white)](android/README.md)
  [![ABI](https://img.shields.io/badge/ABI-arm64--v8a-blue)](android/README.md)
</div>

> [!WARNING]
> This is a development preview, not a finished engine

## What this repository is

This fork brings the open-source OpenXRay 1.6 engine to Android. It uses a native SDL host, Android's Storage Access Framework for game data, OpenAL Soft through OpenSL ES for audio, and Vulkan
renderer being integrated incrementally with the original X-Ray UI and game runtime.

The repository contains **engine source code only**. It does not include or download
commercial S.T.A.L.K.E.R. assets. You must provide your own legally obtained game installation.

## Important info

The engine host now discovers game resources recursively: pick the root of any
installation (SoC, Clear Sky or Call of Pripyat, a repack or a modded layout) and
the archives and the game's own `fsgame.ltx` are located and mounted automatically.
Shadow of Chernobyl is the most tested target; Clear Sky and Call of Pripyat are
experimental. Playing through the full story is not possible yet.

## Device requirements

- Android 8.0 / API 26 or newer;
- 64-bit ARM device (`arm64-v8a`) (Soon arm v7a);
- Vulkan-capable GPU;
- enough storage for the APK, private caches and original game installation;
- a legally obtained S.T.A.L.K.E.R. data set.

The latest development work is tested on a Pixel 7 Pro. Device compatibility is not
yet guaranteed.

## Building

Clone recursively so every pinned dependency is present:

```powershell
git clone --recursive https://github.com/rolipayr15/OpenXray16-Android-Port.git
cd OpenXray16-Android-Port
```

Bootstrap the pinned JDK/SDK/NDK contract and apply the versioned Android compatibility
patches to upstream submodules:

```powershell
./misc/android/bootstrap-sdk.ps1 -AcceptLicenses
```

Then build the ARM64 debug APK:

```powershell
$env:ANDROID_SDK_ROOT = (Resolve-Path ../.toolchains/android-sdk)
./android/gradlew.bat -p android assembleDebug
```

More details and device instructions are in the [Android build guide](android/README.md).

## Installing game data

1. Install the APK on the Android device.
2. Copy your original game installation (or a mod/repack) to accessible device storage.
3. Open the launcher and choose the installation root directory with the system folder picker.
4. Press **Start engine host**.

The launcher walks the selected tree recursively and finds `gamedata.db*`,
`resources/*.db*`, `levels/*.db*`, `patches/*.db*`, `localization/*.db*`, `mp/*.db*`
and the game's own `fsgame.ltx`, so licensed SoC/CS/CoP installs and modded layouts
work without a fixed folder structure. Files that a ransomware has encrypted
(`*.db.ntp`) are skipped safely and reported in the log; replace them with clean
copies from a trusted source.

The permission is retained by Android. The port does not request broad storage access
and does not package, upload or redistribute the selected data.

For native logs:

```powershell
adb logcat -s OpenXRay
```

## Legal and upstream attribution

This is an unofficial fan project and is not affiliated with or endorsed by GSC Game
World. S.T.A.L.K.E.R. names, artwork and game data belong to their respective owners.
Follow the applicable [GSC EULA](https://www.gsc-game.com/eula/) and
[Fan Content Creation Guidelines](https://www.gsc-game.com/guidelines/).

The codebase is derived from [OpenXRay/xray-16](https://github.com/OpenXRay/xray-16).
Its original authors and the wider OpenXRay contributor community retain full credit
for the engine work on which this port is built. See [License.txt](License.txt) for the
source license.
