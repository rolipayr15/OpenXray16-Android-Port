# Changelog

## 0.6.0-alpha.1 — Stage 6 gameplay-bootstrap preview

This release records the first major Android integration snapshot after the original
renderer bootstrap.

### Added

- Native ARM64 host for the complete OpenXRay/ShoC dependency graph.
- Storage Access Framework access to user-owned game data and original encrypted archives.
- Persistent Vulkan swapchain, pipelines, UI geometry and texture rendering.
- DDS decoding, correct font alpha and animated `.seq` cursor textures.
- Original OGM/Theora intros and animated main-menu video textures.
- OpenAL Soft/OpenSL ES audio and synchronized video sequencing.
- ShoC new-game startup through `l01_escape` server and object loading.
- Reproducible Android compatibility patches for upstream submodules.

### Fixed

- Android canvas orientation and input-axis mismatch.
- Resolution divergence between SDL, Vulkan and the ShoC UI.
- Empty main-menu background and missing cursor sprite.
- Incorrectly opaque font atlas rendering.
- Optional main-menu template and initial particle lifecycle blockers.

### Known limitations

- The loaded game world is not rendered and gameplay is not yet usable.
- Touch controls are not available.
- ARMv7 builds are deferred.
- The APK is a development build and carries no game assets.
