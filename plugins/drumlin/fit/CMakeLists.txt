# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# drumlin-fit: fits Drumlin's recipes to recordings (README.md). A tool for
# whoever makes Drumlin's sounds; built with the tests so it keeps compiling,
# never run by them.
find_package(Threads REQUIRED)
add_executable(drumlin-fit fit.cpp)
target_link_libraries(drumlin-fit PRIVATE supple::script-runtime supple::pocketfft supple::dr-libs
                                          Threads::Threads)
target_compile_features(drumlin-fit PRIVATE cxx_std_20)
# As the stock plugins compile their scripts: integers wrap, no fused multiply-adds. The
# sanitizers too, as the runtime it links has them.
supple_compile_options(drumlin-fit PRIVATE -ffp-contract=off -fwrapv ${_supple_sanitize_flags})
target_link_options(drumlin-fit PRIVATE ${_supple_sanitize_flags})
