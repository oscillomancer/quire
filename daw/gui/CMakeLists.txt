# Phase 5 GUI (docs/specs/gui.md, ADR-0005): SDL3 windows (X11 driver; the
# offscreen driver for tests and benchmarks), OpenGL 3.3 core, NanoVG GL3.
find_package(SDL3 REQUIRED CONFIG)
find_package(OpenGL REQUIRED)
find_package(ZLIB REQUIRED)

# NanoVG with its GL3 backend (vendored, Zlib; stb_image inside is PD/MIT).
add_library(gui-nanovg STATIC ${PROJECT_SOURCE_DIR}/third_party/nanovg/src/nanovg.c
                              platform/src/nanovg_gl3.c)
target_include_directories(gui-nanovg SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/nanovg/src)
target_link_libraries(gui-nanovg PUBLIC OpenGL::GL)
set_target_properties(gui-nanovg PROPERTIES POSITION_INDEPENDENT_CODE ON)

supple_add_library(gui-platform
  SOURCES platform/src/window.cpp platform/src/png.cpp
  DEPS supple::core-base
  PRIVATE_DEPS SDL3::SDL3 gui-nanovg ZLIB::ZLIB)
target_include_directories(gui-platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
target_link_libraries(gui-platform PUBLIC gui-nanovg)

# Waveform peak pyramids: builder, disk cache, worker (no GL).
supple_add_library(gui-peaks SOURCES peaks/src/peaks.cpp DEPS supple::audio-file supple::core-base)
target_include_directories(gui-peaks PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/peaks/include)

# The retained toolkit (gui.md §2): nodes, stack layout, input, damage, theme.
supple_add_library(gui-ui
  SOURCES ui/src/theme.cpp ui/src/canvas.cpp ui/src/node.cpp ui/src/root.cpp ui/src/widgets.cpp ui/src/gl.cpp
          ui/src/text_field.cpp ui/src/text_area.cpp ui/src/controls.cpp ui/src/list.cpp ui/src/popup.cpp ui/src/scroll_view.cpp
  DEPS supple::gui-platform supple::core-json
  PRIVATE_DEPS SDL3::SDL3 gui-nanovg)
target_include_directories(gui-ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ui/include)

# Actions, keymaps, macros (gui.md §6): no GL, testable without a window.
supple_add_library(gui-actions SOURCES actions/src/actions.cpp actions/src/keymap.cpp
  DEPS supple::command-bus supple::core-json supple::core-base
  PRIVATE_DEPS SDL3::SDL3)
target_include_directories(gui-actions PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/actions/include)

# Views (gui.md §4): the arrange view (ruler, lanes, TCP column) now; mixer,
# editors follow.
supple_add_library(gui-views SOURCES views/src/lanes.cpp views/src/track_layout.cpp
                                        views/src/arrange.cpp views/src/item_gestures.cpp views/src/tcp.cpp views/src/envelope_lane.cpp views/src/mouse_map.cpp
  DEPS supple::gui-ui supple::gui-peaks supple::model supple::timeline supple::core-params SDL3::SDL3)
target_include_directories(gui-views PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/views/include)

# The app's pieces (shell, generator, actions UI, chrome), shared with tests.
add_library(gui-app STATIC app/shell.cpp app/generate.cpp app/app_actions.cpp app/actions_list.cpp app/chrome.cpp
  app/agent_panel.cpp app/plugin_windows.cpp app/ui_persistence.cpp app/screensets.cpp app/file_actions.cpp app/midi_editor.cpp app/routing_matrix.cpp app/mixer.cpp
  app/fx_params.cpp app/plugin_browser.cpp app/fx_window.cpp app/render_dialog.cpp app/history_panel.cpp app/transport_bar.cpp app/plugin_scales.cpp
  app/midi_event_list.cpp app/appearance.cpp app/track_menu.cpp app/mouse_dialog.cpp)
target_include_directories(gui-app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(gui-app PUBLIC supple::agent supple::gui-actions supple::gui-views supple::gui-ui supple::gui-platform
                      supple::engine supple::control-protocol supple::core-params supple::product-info SDL3::SDL3)
target_compile_definitions(gui-app PUBLIC SUPPLE_SKILL_DIR="${PROJECT_SOURCE_DIR}/skill")
supple_target_defaults(gui-app)

add_executable(${PRODUCT_SLUG} app/main.cpp)
target_link_libraries(${PRODUCT_SLUG} PRIVATE gui-app)
add_dependencies(${PRODUCT_SLUG} ${PRODUCT_SLUG}-bridge)  # isolated fx run in it
target_compile_definitions(${PRODUCT_SLUG} PRIVATE
  SUPPLE_GUI_FONTS_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
supple_target_defaults(${PRODUCT_SLUG})

if(SUPPLE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  supple_add_tests(gui-platform SOURCES tests/test_platform.cpp DEPS SDL3::SDL3 LABELS gui tsan-skip)
  target_compile_definitions(gui-platform.tests PRIVATE
    SUPPLE_GUI_FONTS_DIR="${PROJECT_SOURCE_DIR}/assets/fonts")
  # Mesa's allocations outlive the process by design; leak checks would only
  # report the GL driver.
  set_tests_properties(gui-platform.tests PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  supple_add_tests(gui-peaks SOURCES tests/test_peaks.cpp LABELS gui)
  supple_add_tests(gui-views SOURCES tests/test_views.cpp DEPS gui-app LABELS gui tsan-skip)
  target_compile_definitions(gui-views.tests PRIVATE
    SUPPLE_GUI_FONTS_DIR="${PROJECT_SOURCE_DIR}/assets/fonts"
    SUPPLE_GUI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  set_tests_properties(gui-views.tests PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  supple_add_tests(gui-actions SOURCES tests/test_actions.cpp DEPS supple::engine SDL3::SDL3 LABELS gui)
  # The app driven by keys and clicks, asserting on the session (gui.md §10).
  supple_add_tests(gui-app SOURCES tests/test_app.cpp tests/test_plugin_windows.cpp tests/test_gestures.cpp
                   tests/test_midi_editor.cpp tests/test_fx_render.cpp tests/test_ruler.cpp tests/test_tcp_sends.cpp tests/test_mixer.cpp LABELS gui tsan-skip)
  target_compile_definitions(gui-app.tests PRIVATE SUPPLE_GUI_FONTS_DIR="${PROJECT_SOURCE_DIR}/assets/fonts"
                             SUPPLE_CLAP_DIR="${CMAKE_BINARY_DIR}/plugins/clap"
                             SUPPLE_BRIDGE_PATH="$<TARGET_FILE:${PRODUCT_SLUG}-bridge>"
                             SUPPLE_GUI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  add_dependencies(gui-app.tests ${PRODUCT_SLUG}-bridge)
  target_include_directories(gui-app.tests PRIVATE ${PROJECT_SOURCE_DIR}/daw/agent-panel/tests)
  set_tests_properties(gui-app.tests PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  supple_add_tests(gui-ui SOURCES tests/test_ui.cpp DEPS SDL3::SDL3 LABELS gui tsan-skip)
  target_compile_definitions(gui-ui.tests PRIVATE
    SUPPLE_GUI_FONTS_DIR="${PROJECT_SOURCE_DIR}/assets/fonts"
    SUPPLE_GUI_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  set_tests_properties(gui-ui.tests PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  # The app end to end, offscreen: renders, writes a snapshot, reports timings.
  add_test(NAME gui.app-offscreen
           COMMAND ${PRODUCT_SLUG} --offscreen --no-socket --frames 3 --bench
                   --snapshot ${CMAKE_CURRENT_BINARY_DIR}/app-offscreen.png)
  set_tests_properties(gui.app-offscreen PROPERTIES LABELS "gui;tsan-skip"
                       ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  # --theme and --action (the identity mockups use both): the light theme with
  # the mixer and the history open; an unknown action fails the run.
  add_test(NAME gui.app-theme-actions
           COMMAND ${PRODUCT_SLUG} --offscreen --no-socket --frames 2 --audio none --theme light
                   --action ui.mixer --action ui.history
                   --snapshot ${CMAKE_CURRENT_BINARY_DIR}/app-theme-actions.png)
  set_tests_properties(gui.app-theme-actions PROPERTIES LABELS "gui;tsan-skip"
                       ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  add_test(NAME gui.app-unknown-action
           COMMAND ${PRODUCT_SLUG} --offscreen --no-socket --frames 2 --audio none
                   --action ui.no-such-action)
  set_tests_properties(gui.app-unknown-action PROPERTIES LABELS "gui;tsan-skip" WILL_FAIL TRUE
                       ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  # Named screensets saved into the UI file and recalled in a later run.
  add_test(NAME gui.screensets
           COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/gui_screensets.py
                   $<TARGET_FILE:${PRODUCT_SLUG}> $<TARGET_FILE:${PRODUCT_SLUG}-engine>)
  set_tests_properties(gui.screensets PROPERTIES LABELS "gui;tsan-skip"
                       ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  # An agent drives the running GUI through its control socket and the MCP proxy.
  add_test(NAME gui.agent-over-socket
           COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/integration/gui_agent.py
                   $<TARGET_FILE:${PRODUCT_SLUG}> $<TARGET_FILE:${PRODUCT_SLUG}-mcp>
                   ${CMAKE_CURRENT_BINARY_DIR}/agent-final.png)
  set_tests_properties(gui.agent-over-socket PROPERTIES LABELS "gui;tsan-skip"
                       ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
endif()
