# OpenXRay Android host

This Gradle project builds the open-source Android host only. It never packages commercial S.T.A.L.K.E.R. data.

Bootstrap the pinned SDK/NDK from the repository root, then build the debug APK:

```powershell
./misc/android/bootstrap-sdk.ps1 -AcceptLicenses
$env:ANDROID_SDK_ROOT = (Resolve-Path ../.toolchains/android-sdk)
./android/gradlew.bat -p android assembleDebug
```

Run Android Lint with:

```powershell
./android/gradlew.bat -p android lintDebug
```

The checked-in lint baseline contains findings from the exact SDL2 submodule revision only. New launcher or host findings still fail the task.

The launcher uses Android's Storage Access Framework to retain read access to a directory selected by the user. The URI is passed into native C++ but is not treated as an ordinary filesystem path.

The current host builds `xrCore`, scripting, physics, media/audio, the complete static `xrRender_GL` dependency graph and the complete `xrGame` graph for both ABIs. `libmain.so` calls a small ABI probe compiled inside `xrGame`, so the game target is a checked runtime dependency instead of an unused build side effect. On start the host enumerates the selected SAF root, runs the native dependency smoke suite, initializes the real core filesystem against a generated fixture in app-private storage and then creates the SDL/Vulkan window. This fixture contains no commercial game data. Seekable `gamedata.db*` files are mirrored into the locator through private descriptor-backed symlinks, without copying their bytes.

When a full ShoC archive set is present, the Stage 6 preflight opens the engine configuration, Lua bootstrap and main-menu layout through the archive VFS. It then parses the complete recursive `system.ltx` and `game.ltx` include graphs with the real `CInifile` implementation. No contents are logged or copied. The physical reference installation currently reports 12 archives, 28,164 VFS files, 1,245 system sections and 152 game sections.

OpenAL Soft 1.25.2 is fetched from its official upstream archive with a pinned SHA-256, built as a replaceable shared `libopenal.so`, and configured to require Android's OpenSL ES backend. Utilities, examples, installation targets and network-loaded backends are disabled. A successful device run reports `OpenAL Soft device/context smoke test passed`.

After building the desktop baseline, generate the freely redistributable archive fixture with:

```powershell
./misc/android/build-xrarchive-fixture.ps1
```

The ignored output is `build/android-fixtures/gamedata.db0`. It is intended for local device testing of SAF descriptor access and X-Ray archive discovery; it is not packaged into the APK.

Stage 5 is complete as a renderer-bootstrap gate. The host compiles reviewable GLSL to embedded SPIR-V with the pinned NDK, creates a Vulkan render pass and graphics pipeline, keeps them alive for the full SDL window lifecycle, presents continuously through the swapchain, and stores the driver pipeline cache only in app-private storage. This is a real persistent shader/pipeline/presentation loop, not yet an OpenXRay scene. For native progress, filter logcat by the `OpenXRay` tag; a successful run reports `Presented first persistent Vulkan host frame` followed by periodic `Vulkan host frame loop alive` messages.

The existing OpenXRay GL renderer requests desktop OpenGL 4.1. Zink remains an optional, capability-gated bridge for devices that meet Mesa's requirements; the physical Mali-G57 test device does not. The host therefore selects native Vulkan and never forces a false GL version override. The static GL/R2 renderer is compiled on Android to expose portability failures while its desktop-GL entry points remain disconnected. The next Stage 6 integration boundary is a native Vulkan implementation of the `GEnv.Render`/render-factory interfaces required before `xrGameModule::initialize`, `CGamePersistent`, the menu and a level can run.
