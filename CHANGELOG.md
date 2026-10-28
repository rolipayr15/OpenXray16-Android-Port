# Changelog

## 0.8.0-alpha.1 — Universal game-data discovery + renderer fixes

This release makes the host discover game resources instead of hardcoding the
Shadow of Chernobyl layout, and fixes the most visible renderer defects.

### Added

- Recursive scan of the selected Storage Access Framework tree for game archives
  (`*.db`, `*.db0..dbf`, `*.xdb*`), the game's own `fsgame.ltx`, and loose
  `gamedata` folders — no longer limited to root `gamedata.db*`.
- Universal archive mirroring that recreates the discovered directory layout in
  app-private storage so the engine VFS mounts it exactly as the game expects.
- Game-layout detection for Shadow of Chernobyl, Clear Sky and Call of Pripyat
  (via `fsgame.ltx` `$game_config$` convention + archive markers such as
  `xefis_movies.db` / `resources.db3+`), with the matching `-shoc`/`-cs`/`-cop`
  engine flag selected automatically.
- The game's own `fsgame.ltx` is reused (with only the writable
  `$app_data_root$`/`$logs$`/`$saves$`/`$screenshots$`/`$downloads$` redirected
  to private storage), so repacks and mods with custom `$arch_dir*`/`$game_data$`
  layouts keep working. A synthesized `fsgame.ltx` covers installs without one.
- Encrypted/corrupted archives with a `.db.ntp`/`.xdb.ntp` suffix (typically
  ransomware-encrypted originals) are detected and safely skipped instead of
  crashing the engine, with a clear "replace with a clean copy" log message. A
  file that still carries a valid CFS header is mirrored under its real name.
- Generalized preflight now resolves `system.ltx`/`game.ltx`/`_g.script` through
  `$game_config$`/`$game_data$`, so it passes for all three games.

### Fixed

- Dynamic models no longer render inside-out: the world pipeline front-face is
  now counter-clockwise to match the clip-space Y flip in `world.vert`.
- `model_Delete` releases the cached dynamic mesh, eliminating GPU-buffer leaks
  and dangling `IRenderVisual*` cache keys across despawned objects.
- `ClearDynamicMeshes` batches handles into a single `DestroyWorldMeshes` call
  instead of one full GPU stall per mesh.

### Known limitations

- Loose (unarchived) `gamedata` files are still not mounted through SAF
  descriptors; only archives are currently mirrored. Loose-file overlay support
  is the next discovery milestone.
- Dynamic-model normals are not yet rotated to world space, so sun/hemisphere
  lighting on rotated characters/trees remains approximate.

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
