# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, AU codes, window class, state tag,
# installed file names and test configuration are all generated from it.
#
# PRODUCT_ID, PRODUCT_AU_SUBTYPE and PRODUCT_STATE_TAG identify saved sessions
# and must never change once the product has shipped.

set(PRODUCT_NAME        "Chord")
set(PRODUCT_ID          "com.suppleaudio.chord")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "Plays every note as a chord, optionally strummed.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

# note-effect: notes and MIDI in, notes and MIDI out, no audio. Hosts place
# it before an instrument (AU: a MIDI processor). PRODUCT_CHANNELS stays unset.
set(PRODUCT_KIND        "note-effect")

# Factory presets in product/Presets.h, the user's own, the editor's preset bar.
set(PRODUCT_PRESETS     ON)

# CLAP features after the main category (note-effect, implied by PRODUCT_KIND).
set(PRODUCT_FEATURES    "utility")

# Four ASCII characters each. Supl is Supple Audio's manufacturer code, shared
# by every product; the subtype alone tells products apart.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "ChRd")
set(PRODUCT_STATE_TAG       "ChRd")

set(PRODUCT_DSP_SOURCES     "")
set(PRODUCT_EDITOR_SOURCES  product/Editor.cpp)
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Strum")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
