# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# The factory samples' encoder (render.py): a WAV to 16-bit FLAC with
# libs/audio-file's own encoder, so the same build writes the same bytes.
add_executable(factory-sample-flac flac.cpp)
target_link_libraries(factory-sample-flac PRIVATE supple::audio-file)
supple_target_defaults(factory-sample-flac)
