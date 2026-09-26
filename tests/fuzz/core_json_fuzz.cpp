#include <supple/json/json.h>
#include <cstddef>
#include <cstdint>
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto v = supple::json::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    if (v) {
        auto w = supple::json::write(*v);
        if (w) {
            auto r = supple::json::parse(*w);
            if (!r) __builtin_trap();
        }
    }
    return 0;
}
