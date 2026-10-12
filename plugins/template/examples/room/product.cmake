# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, AU codes, window class, state tag,
# installed file names and test configuration are all generated from it.
#
# PRODUCT_ID, PRODUCT_AU_SUBTYPE and PRODUCT_STATE_TAG identify saved sessions
# and must never change once the product has shipped.

set(PRODUCT_NAME        "Room")
set(PRODUCT_ID          "com.suppleaudio.room")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "A convolution reverb: any impulse response from a file, or its own room.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

set(PRODUCT_KIND        "effect")

# Main bus layouts offered to the host, the first being the default.
set(PRODUCT_CHANNELS    2 1)

# CLAP features after the main category (audio-effect, implied by PRODUCT_KIND).
set(PRODUCT_FEATURES    "reverb" "stereo" "mono")

# Four ASCII characters each. Supl is Supple Audio's manufacturer code, shared
# by every product; the subtype alone tells products apart.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "RoOm")
set(PRODUCT_STATE_TAG       "RoOm")

set(PRODUCT_DSP_SOURCES     "")
set(PRODUCT_EDITOR_SOURCES  product/Editor.cpp)
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Mix")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
