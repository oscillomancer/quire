# What kind of plugin the product is and what it switches on, from
# product/product.cmake, checked and turned into what ProductInfo.h and the
# build need. Everything here is off unless the product asks for it:
#
#   PRODUCT_KIND             effect | instrument | note-effect
#   PRODUCT_CHANNELS         main bus layouts: 1 2 5.1 7.1 7.1.4 ambi1 ambi2 ambi3, or 3..64
#   PRODUCT_SIDECHAIN        ON: an extra audio input, as wide as the main bus
#   PRODUCT_AUX_OUTPUTS      names of extra audio outputs, each as wide as the main bus
#   PRODUCT_NOTE_INPUT       ON: an effect that also takes notes and MIDI
#                            (instruments and note effects always do)
#   PRODUCT_NOTE_OUTPUT      ON: sends notes and MIDI (note effects always do)
#   PRODUCT_NOTE_EXPRESSIONS per-note expressions the product uses, offered to
#                            VST3 hosts: volume pan tuning vibrato expression
#                            brightness pressure
#   PRODUCT_PRESETS          ON: presets -- factory ones from product/Presets.h,
#                            the user's own in a per-platform folder, listed
#                            to CLAP hosts, and the editor's preset bar
#   PRODUCT_TUNING           ON: microtuning for a product with note input --
#                            an MTS-ESP client and the host's CLAP tuning,
#                            through ProcessorBase::retuning()

function(supple_bool out value)
    if (value)
        set(${out} true PARENT_SCOPE)
    else()
        set(${out} false PARENT_SCOPE)
    endif()
endfunction()

# --- kind ------------------------------------------------------------------------------------

if (PRODUCT_KIND STREQUAL "instrument")
    set(PRODUCT_KIND_C Instrument)
    set(main_feature "instrument")
elseif (PRODUCT_KIND STREQUAL "note-effect")
    set(PRODUCT_KIND_C NoteEffect)
    set(main_feature "note-effect")
else()
    set(PRODUCT_KIND_C Effect)
    set(main_feature "audio-effect")
endif()

set(note_input OFF)
if (PRODUCT_NOTE_INPUT OR NOT PRODUCT_KIND STREQUAL "effect")
    set(note_input ON)
endif()
set(note_output OFF)
if (PRODUCT_NOTE_OUTPUT OR PRODUCT_KIND STREQUAL "note-effect")
    set(note_output ON)
endif()
supple_bool(PRODUCT_NOTE_INPUT_C "${note_input}")
supple_bool(PRODUCT_NOTE_OUTPUT_C "${note_output}")

# AU types: a music device, a MIDI processor, an effect that also takes
# MIDI (a "music effect"), or a plain effect.
if (PRODUCT_KIND STREQUAL "instrument")
    set(au_type "aumu")
elseif (PRODUCT_KIND STREQUAL "note-effect")
    set(au_type "aumi")
elseif (note_input)
    set(au_type "aumf")
else()
    set(au_type "aufx")
endif()

set(PRODUCT_FEATURES_C "\"${main_feature}\"")
foreach(feature ${PRODUCT_FEATURES})
    string(APPEND PRODUCT_FEATURES_C ", \"${feature}\"")
endforeach()

# --- layouts ---------------------------------------------------------------------------------

# SUPPLE_LAYOUTS keeps the product's spelling of each, for test names and
# host-smoke's --layout.
set(SUPPLE_LAYOUTS "")
set(layouts_c "")
set(counts_c "")
set(seen "")
if (PRODUCT_KIND STREQUAL "note-effect")
    if (NOT "${PRODUCT_CHANNELS}" STREQUAL "")
        message(FATAL_ERROR "A note effect has no audio: leave PRODUCT_CHANNELS unset")
    endif()
    set(layouts_c "{0, supple::Layout::Generic}")
    set(counts_c "0")
    set(SUPPLE_LAYOUTS none)
endif()
foreach(entry ${PRODUCT_CHANNELS})
    if (entry STREQUAL "1")
        set(count 1)
        set(layout Mono)
    elseif (entry STREQUAL "2")
        set(count 2)
        set(layout Stereo)
    elseif (entry STREQUAL "5.1")
        set(count 6)
        set(layout Surround51)
    elseif (entry STREQUAL "7.1")
        set(count 8)
        set(layout Surround71)
    elseif (entry STREQUAL "7.1.4")
        set(count 12)
        set(layout Surround714)
    elseif (entry MATCHES "^ambi([1-3])$")
        math(EXPR count "(${CMAKE_MATCH_1} + 1) * (${CMAKE_MATCH_1} + 1)")
        set(layout Ambisonic${CMAKE_MATCH_1})
    elseif (entry MATCHES "^[0-9]+$" AND entry GREATER_EQUAL 3 AND entry LESS_EQUAL 64)
        set(count ${entry})
        set(layout Generic)
    else()
        message(FATAL_ERROR "PRODUCT_CHANNELS: '${entry}' is not a layout "
                            "(1 2 5.1 7.1 7.1.4 ambi1 ambi2 ambi3, or 3 to 64 channels)")
    endif()
    # Hosts and the framework tell layouts apart by channel count.
    if (count IN_LIST seen)
        message(FATAL_ERROR "PRODUCT_CHANNELS: two layouts have ${count} channels; keep one")
    endif()
    list(APPEND seen ${count})
    list(APPEND SUPPLE_LAYOUTS "${entry}")
    if (layouts_c)
        string(APPEND layouts_c ", ")
        string(APPEND counts_c ", ")
    endif()
    string(APPEND layouts_c "{${count}, supple::Layout::${layout}}")
    string(APPEND counts_c "${count}")
endforeach()
set(PRODUCT_LAYOUTS_C "${layouts_c}")
set(PRODUCT_CHANNELS_C "${counts_c}")

# --- buses -----------------------------------------------------------------------------------

if (PRODUCT_KIND STREQUAL "note-effect" AND (PRODUCT_SIDECHAIN OR PRODUCT_AUX_OUTPUTS))
    message(FATAL_ERROR "A note effect has no audio: no PRODUCT_SIDECHAIN or PRODUCT_AUX_OUTPUTS")
endif()
supple_bool(PRODUCT_SIDECHAIN_C "${PRODUCT_SIDECHAIN}")
list(LENGTH PRODUCT_AUX_OUTPUTS PRODUCT_AUX_COUNT)
if (PRODUCT_AUX_COUNT GREATER 16)
    message(FATAL_ERROR "PRODUCT_AUX_OUTPUTS: at most 16 extra outputs")
endif()
set(PRODUCT_AUX_NAMES_C "")
foreach(name ${PRODUCT_AUX_OUTPUTS})
    string(LENGTH "${name}" length)
    if (length EQUAL 0 OR length GREATER 31 OR name MATCHES "[\"\\\\]")
        message(FATAL_ERROR "PRODUCT_AUX_OUTPUTS: '${name}' must be 1 to 31 characters with no quotes or backslashes")
    endif()
    string(APPEND PRODUCT_AUX_NAMES_C "\"${name}\", ")
endforeach()
string(APPEND PRODUCT_AUX_NAMES_C "nullptr")

# --- notes ------------------------------------------------------------------------------------

if (PRODUCT_NOTE_EXPRESSIONS AND NOT note_input)
    message(FATAL_ERROR "PRODUCT_NOTE_EXPRESSIONS needs note input (PRODUCT_NOTE_INPUT ON for an effect)")
endif()
# The bits are CLAP's expression IDs, which are also VST3's as clap-wrapper
# numbers them.
set(expression_ids volume pan tuning vibrato expression brightness pressure)
set(PRODUCT_NOTE_EXPRESSIONS_C 0)
foreach(expression ${PRODUCT_NOTE_EXPRESSIONS})
    list(FIND expression_ids "${expression}" bit)
    if (bit LESS 0)
        message(FATAL_ERROR "PRODUCT_NOTE_EXPRESSIONS: '${expression}' is not one of ${expression_ids}")
    endif()
    math(EXPR PRODUCT_NOTE_EXPRESSIONS_C "${PRODUCT_NOTE_EXPRESSIONS_C} | (1 << ${bit})")
endforeach()

# --- presets -----------------------------------------------------------------------------------

supple_bool(PRODUCT_PRESETS_C "${PRODUCT_PRESETS}")
if (NOT PRODUCT_PRESETS AND EXISTS "${SUPPLE_PRODUCT}/Presets.h")
    message(FATAL_ERROR "product/Presets.h lists factory presets: set PRODUCT_PRESETS ON in product.cmake")
endif()

# --- tuning ------------------------------------------------------------------------------------

if (PRODUCT_TUNING AND NOT note_input)
    message(FATAL_ERROR "PRODUCT_TUNING needs note input (an instrument, a note effect, or PRODUCT_NOTE_INPUT ON)")
endif()
supple_bool(PRODUCT_TUNING_C "${PRODUCT_TUNING}")

set(summary "${PRODUCT_KIND}")
if (PRODUCT_CHANNELS)
    list(JOIN PRODUCT_CHANNELS "/" layouts_text)
    string(APPEND summary ", ${layouts_text}")
endif()
if (PRODUCT_SIDECHAIN)
    string(APPEND summary ", sidechain")
endif()
if (PRODUCT_AUX_COUNT GREATER 0)
    string(APPEND summary ", ${PRODUCT_AUX_COUNT} aux output(s)")
endif()
if (note_input)
    string(APPEND summary ", notes in")
endif()
if (note_output)
    string(APPEND summary ", notes out")
endif()
if (PRODUCT_PRESETS)
    string(APPEND summary ", presets")
endif()
if (PRODUCT_TUNING)
    string(APPEND summary ", tuning")
endif()
message(STATUS "${PRODUCT_NAME}: ${summary}")
