# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, AU codes, window class, state tag,
# installed file names and test configuration are all generated from it.
#
# PRODUCT_ID, PRODUCT_AU_SUBTYPE and PRODUCT_STATE_TAG identify saved sessions
# and must never change once the product has shipped.

set(PRODUCT_NAME        "Tone")
set(PRODUCT_ID          "com.suppleaudio.tone")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "A small polyphonic instrument.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

# instrument: notes and MIDI in, one main output, no audio input.
set(PRODUCT_KIND        "instrument")

# Main bus widths offered to the host, the first being the default.
set(PRODUCT_CHANNELS    2 1)

# The per-note expressions the processor uses, offered to VST3 hosts too
# (CLAP hosts may send any; noteExpression() ignores the rest).
set(PRODUCT_NOTE_EXPRESSIONS tuning volume)

# Microtuning: an MTS-ESP master's tuning, or the host's (CLAP tuning),
# through retuning().
set(PRODUCT_TUNING      ON)

# CLAP features after the main category (instrument, implied by PRODUCT_KIND).
set(PRODUCT_FEATURES    "synthesizer" "stereo")

# Four ASCII characters each. Supl is Supple Audio's manufacturer code, shared
# by every product; the subtype alone tells products apart.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "ToNe")
set(PRODUCT_STATE_TAG       "ToNe")

set(PRODUCT_DSP_SOURCES     "")
set(PRODUCT_EDITOR_SOURCES  product/Editor.cpp)
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Level")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
