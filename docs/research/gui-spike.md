# SDL3/XWayland GUI shell spike (2026-09-26)

This is evidence for ADR-0005, not a DAW GUI implementation or an architecture decision. Source is in `tools/spikes/gui-shell/`. It builds only with `SUPPLE_BUILD_SPIKES=ON`; the root option defaults OFF. The spike links system SDL3, X11 and GL, the vendored MIT CLAP headers and Zlib NanoVG source. `main.cpp` is original, using the template's MIT `tools/linux_host_probe.cpp` as a reference for CLAP embedding mechanics. No template files were changed.

## Build and local environment

Commands from the repository root:

```sh
cmake -S . -B build/dev -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Debug -DSUPPLE_WERROR=ON -DSUPPLE_BUILD_SPIKES=ON
cmake --build build/dev --target gui-shell-spike -j6
```

Both passed with Clang 22.1.8. The binary is `build/dev/tools/spikes/gui-shell/gui-shell-spike`. A sandboxed `--draw --frames 2` failed `SDL_Init failed: x11 not available`; the runs below used escalation to access the existing XWayland session and closed all windows/threads afterward. Each mode printed `video_driver=x11`, `display_scale=1.000`, `window_scale=1.000`, `GL_VENDOR=Intel`, `GL_RENDERER=Mesa Intel(R) Arc(tm) B580 Graphics (BMG G21)`, `GL_VERSION=4.6 (Core Profile) Mesa 26.2.3-arch1.1`. SDL requested a 3.3 core context and received 4.6 core. These are SDL-reported scale values on the current XWayland display, not a survey of other monitors/scaling configurations.

## Draw benchmark

```sh
build/dev/tools/spikes/gui-shell/gui-shell-spike --draw --frames 60 --size 1920x1080
```

The deterministic mock has 64 track rows, three header buttons and eight items per row; each item draws a 1,500-point waveform path. A ruler and track labels use Noto Sans. All 64 rows are submitted to NanoVG even when clipped from the 1080-pixel viewport; this is intentionally a heavy baseline, not a culled production renderer. Swap interval is set to zero. Process CPU time is `getrusage(RUSAGE_SELF)` around NanoVG drawing and `SDL_GL_SwapWindow`; GPU-complete wall time is steady-clock time from frame start through `glFinish()` after the swap. Neither measure isolates GPU execution alone.

| Measure | Median | p95 | Max |
| --- | ---: | ---: | ---: |
| Process CPU per frame | 81.489 ms | 82.528 ms | 97.739 ms |
| Wall time through `glFinish` | 101.566 ms | 107.521 ms | 130.639 ms |

The 60 Hz budget is 16.67 ms, so this rendering approach **as implemented** misses it by a large margin. Culling, waveform min/max aggregation, cached geometry, fewer NanoVG path commands and proper viewport profiling are needed before using this as evidence that the proposed rendering architecture meets DAW performance targets. The benchmark does show the GL3 backend runs on the B580 under XWayland. Retained mode here means retaining the UI model and waveform data, not that NanoVG retains GPU draw commands; this spike rebuilds the paths each frame.

## Event-driven idle

```sh
build/dev/tools/spikes/gui-shell/gui-shell-spike --idle --seconds 5
```

`SDL_WaitEventTimeout` wakes on events and redraws only for expose/resize. Output: `idle_seconds=5 process_cpu_seconds=0.002452 events=17 redraws=3` (about 0.05% of one CPU). The SDL/XWayland shell can idle nearly without CPU use when no animation or audio-driven redraw is requested.

## CLAP editor embedding, resizing and capture

```sh
build/dev/tools/spikes/gui-shell/gui-shell-spike --embed /home/nate/Projects/audio-plugins/template/build-cmake/starter_assets/Starter.clap --seconds 1
magick docs/research/gui-spike/embedded.ppm docs/research/gui-spike/embedded.png
```

The process `dlopen`s the bare `.clap`, creates/initializes/activates it at 48 kHz, block 512, and runs `process()` on two silent float channels on a separate thread. The SDL3 window's `SDL_PROP_WINDOW_X11_WINDOW_NUMBER` supplies the parent XID. The spike creates an X11 child container, sets `CLAP_WINDOW_API_X11`, calls `gui->set_parent/show`, and supplies `clap.timer-support` from the SDL event loop. `XQueryTree` found one mapped plugin child under the container in **3/3** create/destroy cycles. Both programmatic CLAP resizes returned true; the editor was fully visible after reconciling the container to the actual X11 size. The host logged 339 timer ticks, `audio_ok=true`, and `timer_after_close=4294967295` (`CLAP_INVALID_ID`) on all three closes. The audio thread was joined and the plugin/window/dynamic library were torn down. No real audio device was opened.

Hyprland's tiling overrides requested top-level geometry asynchronously: the initial request was 1100×760 while immediate SDL/X11 observations differed, and both later requests (1360×900, 1160×780) settled to an actual 1259×1381 X11/SDL window. The code now resizes the child container against the *observed* top-level dimensions and calls `gui->adjust_size/set_size`; the final plugin size was 1219×714. A host must react to compositor resize/configure events instead of assuming `SDL_SetWindowSize` was honored. The plugin child stayed embedded in the SDL parent and was not promoted to a separate top-level window.

`XGetImage` on the SDL XID produced [the full PNG](gui-spike/embedded.png) and [a cropped detail](gui-spike/embedded-detail.png). It captured the plugin child pixels, establishing actual rendering. In the final capture, the SDL OpenGL header appears black and the tiling-sized window has unused lower space; an earlier capture did contain the header. Thus XGetImage on an XWayland/GL parent is not a reliable proof of the composited top-level image after resize. The screenshot is evidence for child embedding, not an exact visual record of all GL layers. The intermediate PPM was removed after PNG conversion. Focus handoff and keyboard events inside the plugin editor were not exercised.

## SDL text input

```sh
build/dev/tools/spikes/gui-shell/gui-shell-spike --text --seconds 3
```

Output: `SDL_StartTextInput=true`, `text_events=0`; no synthetic input was generated, and `xdotool` was not installed. The SDL API setup succeeds on X11; actual text event delivery and focus behavior are still unverified.

## Build gate and conclusion

`tools/ci/format.sh` ran; it also reformatted two unrelated core-base headers, which were restored to avoid changing another task. `clang-format --dry-run -Werror tools/spikes/gui-shell/main.cpp tools/spikes/gui-shell/nanovg_gl3.c` passed. The repository-wide `tools/ci/format.sh --check` currently fails on those pre-existing core-base headers. `cmake --workflow --preset dev` passed. The latter ran the license gate (`OK, 5 dependencies`) and two CTest tests (`core-base.tests`, `license-check`, both passed) with the spike enabled in `build/dev`. A separate default-off configure/build/test in `/tmp/quire-gui-default` passed the same two tests with `-DSUPPLE_BUILD_SPIKES=OFF`, proving the optional target does not affect the default build.

The intended X11 SDL3/GL3/NanoVG path is technically viable for window creation, event-driven idle and CLAP X11 child embedding on this machine. This spike does **not** validate its performance for the proposed dense arrange view: the naive NanoVG path load is far over the frame budget. Before accepting the full GUI ADR, measure visible-row culling and cached waveform geometry, verify focus/keyboard input into the plugin, test fractional XWayland scale, and capture the composited window with a reliable method. Pugl comparison was omitted because the required SDL/embedding evidence already exposed the main open questions.
