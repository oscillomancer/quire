# Every third-party source the framework builds from, pinned. Read by
# Dependencies.cmake when configuring and by tools/vendor.cmake when making
# an offline copy, so both always agree on what a build is made of.
#
#   SUPPLE_PIN_<name>          repository and commit (or tag, for the SDKs
#                              clap-wrapper pins itself)
#   SUPPLE_PIN_<name>_MODULES  git submodules a checkout needs
#   SUPPLE_PIN_<name>_WHEN     when it is needed: always, plugin, gui, metal
#                              (macOS), tuning (PRODUCT_TUNING), auv2 (macOS)
#
# clap, vst3sdk and AudioUnitSDK are clap-wrapper's own pins, repeated here
# so a vendored copy can supply them; Dependencies.cmake stops the configure
# if clap-wrapper's base_sdks.cmake ever names different ones. Revalidate
# every format before moving any of these, and regenerate NOTICES.txt when
# one moves.

set(SUPPLE_PINS dr_libs clap-wrapper clap vst3sdk AudioUnitSDK mts-esp pugl nanovg nanosvg nanovg_metal)

set(SUPPLE_PIN_dr_libs https://github.com/mackron/dr_libs.git dfe8377631000664666519fdb83da193fd8037f4)  # dr_wav 0.14.6, dr_flac 0.13.4
set(SUPPLE_PIN_dr_libs_WHEN always)

set(SUPPLE_PIN_clap-wrapper https://github.com/free-audio/clap-wrapper.git 1cca996e96f29ab2be7ae9f8cfe532bbc92e1dd6)  # v0.16.0
set(SUPPLE_PIN_clap-wrapper_WHEN plugin)
set(SUPPLE_PIN_clap https://github.com/free-audio/clap.git 1.2.6)
set(SUPPLE_PIN_clap_WHEN plugin)
set(SUPPLE_PIN_vst3sdk https://github.com/steinbergmedia/vst3sdk.git v3.8.0_build_66)
set(SUPPLE_PIN_vst3sdk_MODULES base public.sdk pluginterfaces cmake)
set(SUPPLE_PIN_vst3sdk_WHEN plugin)
set(SUPPLE_PIN_AudioUnitSDK https://github.com/apple/AudioUnitSDK.git AudioUnitSDK-1.1.0)
set(SUPPLE_PIN_AudioUnitSDK_WHEN auv2)

set(SUPPLE_PIN_mts-esp https://github.com/ODDSound/MTS-ESP.git f214739b8832e7f297cb9970d0c0efbf783f1462)
set(SUPPLE_PIN_mts-esp_WHEN tuning)

set(SUPPLE_PIN_pugl https://github.com/lv2/pugl.git b7637149ebe53124e5be90559e02a0185bbcbd73)
set(SUPPLE_PIN_pugl_WHEN gui)
set(SUPPLE_PIN_nanovg https://github.com/memononen/nanovg.git ce3bf745eb2d2dbc14a50bf2446783f691ac4353)
set(SUPPLE_PIN_nanovg_WHEN gui)
set(SUPPLE_PIN_nanosvg https://github.com/memononen/nanosvg.git 239e102ec2c691f2902e20ace2ed36ee4a35cfe6)
set(SUPPLE_PIN_nanosvg_WHEN gui)
# wjakob's fork rather than ollix/MetalNanoVG (untouched since 2022): it adds
# an explicit command queue and target texture, and sizes the stencil buffer
# from the render target, which NVG_STENCIL_STROKES needs. Shaders ship
# precompiled, so there is no .metal step to own.
set(SUPPLE_PIN_nanovg_metal https://github.com/wjakob/nanovg_metal.git 1e4243990741f5b0da73bc0f25c85ae0e31b8766)
set(SUPPLE_PIN_nanovg_metal_WHEN metal)

# Patches, by the dependency they apply to (cmake/patches/).
set(SUPPLE_PATCHES_clap-wrapper
    clap-wrapper-vst3-all-points.patch clap-wrapper-vst3-timer-floor.patch clap-wrapper-vst3-self-handle.patch
    clap-wrapper-vst3-surround.patch clap-wrapper-vst3-midi-out.patch clap-wrapper-vst3-tail-on-main.patch
    clap-wrapper-vst3-note-capacity.patch clap-wrapper-auv2-restart.patch clap-wrapper-auv2-state-errors.patch
    clap-wrapper-auv2-bypass-sync.patch)
set(SUPPLE_PATCHES_pugl pugl-win-module-class.patch pugl-mac-drop-location.patch pugl-reject-offer.patch)

# What a vendored checkout records about itself (tools/vendor.cmake writes
# it, Dependencies.cmake compares it): the repository and the pin.
function(supple_pin_stamp name out)
    list(GET SUPPLE_PIN_${name} 0 url)
    list(GET SUPPLE_PIN_${name} 1 ref)
    string(STRIP "${url} ${ref} ${SUPPLE_PIN_${name}_MODULES}" stamp)
    set(${out} "${stamp}" PARENT_SCOPE)
endfunction()
