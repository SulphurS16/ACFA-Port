# Experimental Apple Silicon / Metal port

This branch keeps ACFA-Port's static-recompilation architecture and adds a native
macOS host/backend. It is based on upstream 0.7.2, commit
`09e5a3c1a147983ebfd2e7fcb0a899421f43ebb1`. This is a source contribution for
maintainer review, not a packaged Mac release or a replacement game engine.

## Compatibility and evidence

| Content / platform | Scope and verification |
| --- | --- |
| Japanese ACFA | Exact `BLJM55005`, disc 01.02; manually played on Apple M1 Max, 64 GB, including menus, saves and combat. This does not cover every Japanese product ID or executable revision. |
| Chinese community translation | The locally installed community Chinese resource translation on that same Japanese executable was used for testing. It loads the existing translated assets; this PR does not translate or distribute them. An executable-modifying translation with a different SHA is not automatically accepted. |
| English / US ACFA | The existing `BLUS30187` profile and US/Windows defaults are retained. The Mac backend is intended to support it, but English/US gameplay on Mac has **not** been tested. JP-specific hook addresses must not be copied to the US executable. |
| Windows | Existing backend retained behind platform guards; no Windows build or in-game regression test was available for this contribution. |
| Linux / Android / Intel Mac | Not implemented or validated by this contribution. |

The JP decrypted EBOOT must match
`ee03439ea2cbda0de31b32848bc8b686c60dc0761b54db25be7281afbe20d5d2`.
Input SHA checks are preserved. Do not change a hash merely to accept an unknown
revision. Firmware inputs remain those checked by `known_inputs.cpp`.

## Architecture

- **CPU:** the existing PPU/SPU recompilers still emit C++; Apple Clang builds
  ARM64 code. SIMDe 0.8.2 preserves the guest vector layout and supplies the x86
  intrinsic vocabulary. No generated game code is committed.
- **Graphics:** the existing RSX decoder and HLSL translators feed Slang, then
  native MSL/Metal. The internal D3D11-shaped vocabulary is a small native adapter;
  Windows D3D11 itself is not running through Wine or a Windows emulator.
  Raw vertex buffers are remapped to Metal buffer slots 16–19, keeping constant
  slots separate. Shader identity, stage, entry and compiler/binding ABI participate
  in cache identity; damaged cache payloads are rejected.
- **Host services:** AppKit/CAMetalLayer windows, AudioUnit output, GameController
  input, pthread/time support, macOS VM mapping, CoreFoundation RTC/timezone and
  CoreText/CoreGraphics UI rendering. The regular controller-driven settings and
  system dialogs remain; there is no always-on FPS/CPU/GPU/resolution HUD.
- **JP profile:** independent executable identity, title/language/save paths,
  verified hook map and SPU position manifest. The SPU manifest is metadata only;
  the actual instructions are reconstructed from the user's matching files.
- **Controls:** Mac and JP builds use Xbox/PlayStation-style controllers. DualSense
  basic menu/game input was tested; Xbox support is implemented but not physically
  tested here. Nintendo controllers and Mac rumble are not included. Keyboard/mouse
  gameplay and camera hooks are disabled for these configurations. US/Windows
  defaults retain upstream keyboard/mouse behavior.

### Memory and compatibility fixes

macOS can use 16 KiB host pages while the guest uses 4 KiB pages. The VM layer
tracks guest subpages separately, preserves neighboring data and zero-on-recommit,
and uses whole host-page stack guards. This is not per-4-KiB hardware protection
inside a shared host page. macOS write-watch is not implemented; immutable GPU
reuse therefore requires content validation rather than assuming memory is clean.

The HLE callback registry uses stable storage so appending callbacks cannot
invalidate another thread's import pointer. Reservation waits retain their
predicate/notification ordering. Native Metal resource lifetimes include queued
work, and autorelease pools cover long-lived non-AppKit worker threads.

GameController events are consumed through a serial handler queue and cached
snapshots instead of unsafely polling live profiles from unrelated threads.
Device disconnect clears state; stale callbacks cannot update a replacement slot.
Menu/Options delivery uses the native controller system-gesture API.

## Optimization work included

These changes address host submission and repeated data processing; low total
CPU/GPU utilization alone was not treated as proof that either processor was idle.

| Change | Implementation and correctness boundary |
| --- | --- |
| Render-pass batching | Reuse compatible Metal encoders instead of opening a load/store pass per draw. Clear, copy, feedback, query, attachment changes and present remain boundaries. Guest draw order is preserved. |
| Pipeline lookup | A last-pipeline fast path handles unchanged state; an explicit canonical key handles alternating equivalent state. Descriptor creation occurs on a real cache miss, not every lookup. Shader functions remain strongly retained. |
| Resource binding reuse | Memoize actual native buffer/texture/sampler identities and offsets within the encoder. A new encoder, COW/discard allocation or state reset invalidates the relevant memo. |
| Small constant buffers | Exclusive constant buffers up to 4096 bytes use per-draw `setVertexBytes` / `setFragmentBytes` snapshots and CPU revision tracking. Later updates cannot change an already queued draw. GPU-copy participants and larger/other buffers keep the conservative path. The earlier general constant `memcmp` experiment was replaced after poor real-workload hit rates. |
| Vertex streams on macOS | Bounded exact-content snapshots validate **every use**, including same-frame changes. Stable streams can use immutable native buffers; changed or over-budget data falls back to upload. CPU snapshot budget is 64 MiB, per-stream limit 2 MiB. Index processing is not claimed to be fully cached. |
| Texture validation | Bounded exact comparisons avoid repeated serial hashing/conversion for unchanged textures. CPU snapshot budget is 192 MiB, per-texture limit 16 MiB; allocation failures/over-budget entries retain the fallback. |
| Shader startup | Prewarm verified local cache records with count/time budgets; retain up to 512 native shader modules with the source-byte cap. This reduces previously encountered compilation stalls, not compilation of every possible unseen game shader. |

No distance culling, disabled destructible scenery, reduced collision/AI updates,
or far-texture mip/blur experiment is included. The tested output used a 1920×1080
internal scene; a user's resolution settings are still honored.

Synthetic measurements from the same development machine illustrate the CPU
submission changes, **not guaranteed game FPS**: a 500-draw alternating-state
case dropped from about 4.60 ms to 0.52 ms CPU encoding after canonical pipeline
lookup; a constant-update case dropped from about 2.79 ms to 0.37 ms after small
constant snapshots. Full-image checks were retained. Similar draw-count gameplay
samples showed roughly 17 MiB versus 4 MiB vertex ring uploads, but these were not
strictly identical-scene A/B runs. The tester reported substantial improvement
across repeated seven-AC HARD battle runs. Heavy-scene drops and occasional
presentation waits remain; this is not a stable-60-FPS claim.

## Newly saved blueprint thumbnail fix

Old imported previews displayed correctly; newly saved previews contained colored
noise. In `DESDOC.DAT`, slots are `0x5ED8` bytes after a `0x148` header. The 256×128
preview starts at slot `+0x1ED4` and contains 16384 bytes of standard linear BC1.
Three new saves contained identical bad preview bytes while old slots were intact.
Changing DXT byte order or the save reader would have hidden the real problem.

The game also reads rendered textures directly on the PPU before CPU DXT
compression. Synchronizing only NV0039/NV3089 transfer sources did not cover that
path. The exact JP hooks now scope `TextureCompressor` (`0x003fdfa0`) and its
successful level-zero read lock (`0x00d60f30`). They request the source pixels on
the FIFO thread after GET catches PUT, finish preceding native GPU work, and copy
the dirty color surface into guest ARGB memory before the original compressor
reads it. Ordinary texture locks and destination/write locks do not wait.

Queued readback requests have a bounded wait and can be cancelled before execution;
a cancelled request cannot write later into reused memory. Once a GPU copy has
started, the caller retains its memory until completion. The decoder, compressor,
save layout and blueprint parameters are unchanged. The tester confirmed that a
newly saved preview was fixed. Already corrupted previews need re-saving; they
are not silently replaced with a stock picture. The US counterpart hook needs its
own mapping and testing before claiming the same fix there.

## Building and reproducing

Requirements: Apple Silicon, macOS SDK/Command Line Tools, CMake 3.25+, Python 3,
and the pinned Slang 2026.20 ARM64 SDK. No global tool or emulator configuration
is rewritten. The Slang fetcher checks the archive SHA, CRC, extraction paths and
compiler version; the SDK stays outside the source tree.

From the source root:

```sh
python3 tools/fetch-slang-macos.py
sh tools/build-macos-tools.sh
```

The script selects JP/controller-only and runs CTest. To select the retained US
profile explicitly, use `-DACFA_GAME_PROFILE=us-blus30187` in a separate CMake
build directory. On Windows the default profile is US and controller-only is off;
macOS runtime builds require controller-only.

Prepare your own decrypted inputs using `acfa-prep import` (same verified-input
route as upstream). For example, with paths appropriate to your own extraction:

```sh
../local/build-host/acfa-prep import --disc "$DISC" --eboot "$EBOOT_ELF" \
  --modules-dir "$MODULES" --flash-file "$FLASH_ATRAC" \
  --out ../local/prepared/BLJM55005 --flash-out ../local/dev_flash
```

Pass `--game-data "$GAME_DATA"` if using a matching installed update/translation
resource directory. The preparation output and game/firmware stay local.
Generate PPU/LLE code with the profile's hooks and rebuild SPU records/code:

```sh
sh tools/prepare-macos-codegen.sh ../local/prepared/BLJM55005
sh tools/build-macos-runtime.sh
../local/build-codegen/acfa --help
```

Equivalent runtime configuration uses `ACFA_BUILD_RUNTIME=ON`,
`ACFA_EXPERIMENTAL_MAC_RUNTIME=ON`, `ACFA_GAME_PROFILE=jp-bljm55005`,
`ACFA_CONTROLLER_ONLY=ON`, `ACFA_GEN_DIR` and `ACFA_SPU_GEN_DIR` pointing at your
local generated sources. `ACFA_SLANG_ROOT` can point to an already verified SDK.
Mac runtime linking remains an explicit experimental opt-in.

A typical developer launch after preparation is:

```sh
../local/build-codegen/acfa --data "$INSTALL" --disc "$DISC" \
  --flash ../local/dev_flash --user "$TEST_USER" --windowed --pacing clock
```

Use a separate test save directory. `--help` describes paths/options; it does not
prove gameplay compatibility. There is no portable Mac application installer in
this contribution. The local tester's machine-specific shortcut, configuration,
paths, caches and saves are excluded.

## Verification and remaining work

The source suite contains 16 CTest entries: hashes/input/profile policies, VM,
actual headless Metal shader execution, resources/context/batching, display/UI,
audio, threads, concurrent PPU/HLE core, VFS, shader prewarm and synthetic RSX
through the real renderer/Recorder/native GPU. Tests compare actual pixels and
exercise COW/discard/copy lifetimes, texture invalidation and readback. Fixtures
are handwritten and do not need game code or firmware.

Manual evidence covers the local Japanese/community-Chinese setup, controller
input, combat and the corrected new blueprint preview. It is not a full-game,
all-controller, all-firmware or multiplayer certification. Outstanding work:
US/English Mac gameplay and hook mapping, Windows regression, more hardware and
macOS versions, controller reconnect/rumble coverage, resize/mixed-DPI handling,
remaining presentation stalls and isolated guest crashes. SPU position metadata
includes conservative interpreter fallback and is not complete runtime coverage.

Implementation and tests were developed with substantial OpenAI Codex assistance;
the contributor performed manual in-game tests. Maintainer review is requested,
and individual parts may be split, changed or rejected. The upstream MIT license
is retained; vendored SIMDe carries its own license. No game/firmware bytes,
translated assets, generated PPU/SPU code, shader cache or private runtime logs
are part of this source contribution.
