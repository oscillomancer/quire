// The DSP script library (ADR-0032), shared by the engine and the sandbox: a
// script is <library>/<name>/<name>.cpp, and the script player lists a
// plugin for each one in the folders SUPPLE_SCRIPT_LIBRARY names.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

namespace supple::dsp_commands {

// Names `dir` to the script player: read in this process, and inherited by
// the scanner and bridge processes. It comes first, then any folders the
// environment named when the process started.
inline void nameScriptLibrary(const std::filesystem::path& dir) {
    static const std::string inherited = [] {
        const char* v = std::getenv("SUPPLE_SCRIPT_LIBRARY");
        return std::string(v ? v : "");
    }();
    std::string value = dir.string();
    if (!inherited.empty()) value += ":" + inherited;
    ::setenv("SUPPLE_SCRIPT_LIBRARY", value.c_str(), 1);
}

}  // namespace supple::dsp_commands
