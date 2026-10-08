#include "supple/base/thread.h"

#include <pthread.h>

#include <algorithm>
#include <cstring>

namespace supple {

void nameThread(std::string_view name) {
    char text[16] = {};  // the kernel's TASK_COMM_LEN, with its terminator
    std::memcpy(text, name.data(), std::min(name.size(), sizeof(text) - 1));
    ::pthread_setname_np(::pthread_self(), text);
}

}  // namespace supple
