# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, AU codes, window class, state tag,
# installed file names and test configuration are all generated from it.
#
# PRODUCT_ID, PRODUCT_AU_SUBTYPE and PRODUCT_STATE_TAG identify saved sessions
# and must never change once the product has shipped.

set(PRODUCT_NAME        "Drive")
set(PRODUCT_ID          "com.suppleaudio.drive")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "A saturator, with the editor the framework makes from its parameters.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

# effect: audio in, audio out.
set(PRODUCT_KIND        "effect")

# Main bus widths offered to the host, the first being the default.
set(PRODUCT_CHANNELS    2 1)

# Factory presets (product/Presets.h), and the user's own.
set(PRODUCT_PRESETS     ON)

# CLAP features after the main category (audio-effect, implied by PRODUCT_KIND).
set(PRODUCT_FEATURES    "distortion" "stereo" "mono")

# Four ASCII characters each. Supl is Supple Audio's manufacturer code, shared
# by every product; the subtype alone tells products apart.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "DrIv")
set(PRODUCT_STATE_TAG       "DrIv")

set(PRODUCT_DSP_SOURCES     "")
# No editor code: the framework lays every parameter out by group
# (framework/gui/AutoEditor.cpp). List product/Editor.cpp here to replace it.
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Drive")
set(PRODUCT_SMOKE_LATENCY_PARAM "Quality")
