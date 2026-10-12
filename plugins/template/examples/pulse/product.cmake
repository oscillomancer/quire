# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, AU codes, window class, state tag,
# installed file names and test configuration are all generated from it.
#
# PRODUCT_ID, PRODUCT_AU_SUBTYPE and PRODUCT_STATE_TAG identify saved sessions
# and must never change once the product has shipped.

set(PRODUCT_NAME        "Pulse")
set(PRODUCT_ID          "com.suppleaudio.pulse")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "A tremolo and auto-pan that follows the host's tempo.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

# effect: one main input and output bus of equal width.
set(PRODUCT_KIND        "effect")

# Main bus widths offered to the host, the first being the default.
set(PRODUCT_CHANNELS    2 1)

# Factory presets in product/Presets.h, the user's own, the editor's preset bar.
set(PRODUCT_PRESETS     ON)

# CLAP features after the main category (audio-effect, implied by PRODUCT_KIND).
set(PRODUCT_FEATURES    "tremolo" "stereo" "mono")

# Four ASCII characters each. Supl is Supple Audio's manufacturer code, shared
# by every product; the subtype alone tells products apart.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "PuLs")
set(PRODUCT_STATE_TAG       "PuLs")

set(PRODUCT_DSP_SOURCES     "")
set(PRODUCT_EDITOR_SOURCES  product/Editor.cpp)
# Compiled in, found by file name: the logo and the light/dark switch's icons.
set(PRODUCT_ASSETS          product/assets/logo.svg product/assets/sun.svg product/assets/moon.svg)
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Depth")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
