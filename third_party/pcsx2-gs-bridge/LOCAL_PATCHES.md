# Local changes to PCSX2 in the graphics DLL

The graphics DLL is built from PCSX2 2.8.2 (see PINNED_REVISION) plus the bridge's own patch and these changes:

- `pcsx2/GS/Renderers/HW/GSRendererHW.cpp`: `Destroy`, `PurgeTextureCache`, `ReadbackTextureCache` and `Reset` check that
  `g_texture_cache` exists before using it. A reset or purge arriving while the cache does not exist read through a null pointer.
- `pcsx2/GS/Renderers/Common/GSRenderer.cpp` (`SaveSnapshotToMemory`, v0.4.2, `patches/GSRenderer-snapshot-4k8k.patch`):
  the picture brought back to the CPU every frame is capped at `RRV_PCSX2_GS_SNAPSHOT_MAX` (the game sets it to the
  largest screen's size, shape kept), shrunk in 2:1 steps so no detail is skipped (`RRV_PCSX2_GS_SNAPSHOT_HALVING=0`
  turns the steps off), and the staging texture is kept between frames. At 8K the full-size copy was ~165 MB a frame.
- `gsrunner_host.cpp`: `RRV_PCSX2_GS_LOG_FILE=<path>` writes PCSX2's own log to a file.
- `pcsx2/GS/Renderers/HW/GSTextureReplacements.cpp` (v0.5.2, `patches/GSTextureReplacements-bridge-serial.patch`): the bridge
  has no VM, so the game's serial (the folder under `textures/`) falls back to `RRV_PCSX2_GS_TEXTURE_SERIAL`. `bridge.cpp`
  turns texture replacement on with `RRV_PCSX2_GS_TEXTURE_REPLACE=1`, dumping with `RRV_PCSX2_GS_TEXTURE_DUMP=1`, and
  sets the folder with `RRV_PCSX2_GS_TEXTURES_DIR` (HD texture packs, e.g. an HD HUD).
