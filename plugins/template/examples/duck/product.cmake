# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, AU codes, window class, state tag,
# installed file names and test configuration are all generated from it.
#
# PRODUCT_ID, PRODUCT_AU_SUBTYPE and PRODUCT_STATE_TAG identify saved sessions
# and must never change once the product has shipped.

set(PRODUCT_NAME        "Duck")
set(PRODUCT_ID          "com.suppleaudio.duck")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "A ducker keyed from its sidechain, with what it removes on a second output.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

set(PRODUCT_KIND        "effect")

# Main bus layouts offered to the host, the first being the default: stereo,
# mono, 5.1, 7.1.4 and first-order ambisonics. Ducking applies one gain to
# every channel, so it means the same in each.
set(PRODUCT_CHANNELS    2 1 5.1 7.1.4 ambi1)

# Capabilities (see framework/cmake/Capabilities.cmake): a sidechain input,
# as wide as the main bus, and one extra output carrying what was ducked
# away, so the two outputs sum to the input.
set(PRODUCT_SIDECHAIN   ON)
set(PRODUCT_AUX_OUTPUTS "Ducked")

# CLAP features after the main category (audio-effect, implied by PRODUCT_KIND).
set(PRODUCT_FEATURES    "compressor" "stereo" "mono" "surround" "ambisonic")

# Four ASCII characters each. Supl is Supple Audio's manufacturer code, shared
# by every product; the subtype alone tells products apart.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "DuCk")
set(PRODUCT_STATE_TAG       "DuCk")

set(PRODUCT_DSP_SOURCES     "")
set(PRODUCT_EDITOR_SOURCES  product/Editor.cpp)
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Depth")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
