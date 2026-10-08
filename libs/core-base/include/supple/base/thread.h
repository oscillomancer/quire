// Names for the threads the host owns (engine.md §2.2). Linux. Setup only:
// never call from an audio path.
#pragma once

#include <string_view>

namespace supple {

// Names the calling thread for `ps -L`, `top -H`, debuggers and profilers.
// Linux keeps at most 15 bytes; a longer name is cut there.
void nameThread(std::string_view name);

}  // namespace supple
