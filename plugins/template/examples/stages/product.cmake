# Product identity and sources. This is the only place the product is named:
# the CLAP descriptor, bundle identifiers, window class, state tag, installed
# file names and test configuration are all generated from it.
#
# PRODUCT_ID and PRODUCT_STATE_TAG identify saved sessions and must never
# change once the product has shipped.

set(PRODUCT_NAME        "Stages")
set(PRODUCT_ID          "com.suppleaudio.stages")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "Gain stages, as many as a file names: a run-time product.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

# What its plugins are is decided when it runs (product/Catalog.cpp): one is
# built in, one more for each file in a folder. The kind, layouts and
# features below are the built-in one's, which host-smoke tests. A run-time
# product builds CLAP and VST3; an AU component is a single plugin.
set(PRODUCT_RUNTIME     ON)
set(PRODUCT_KIND        "effect")
set(PRODUCT_CHANNELS    2 1)
set(PRODUCT_FEATURES    "utility")

# Its layer (product/Layer.cpp): reloads a plugin whose file changed, keeps
# values by stage name in the state, and answers a CLAP extension of its own.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

# Four ASCII characters each; the AU codes are unused by a run-time product
# but name it all the same.
set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "StGs")
set(PRODUCT_STATE_TAG       "StGs")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# No editor code: the automatic one, laid out from each plugin's table.
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM "Output")
