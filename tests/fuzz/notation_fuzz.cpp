#include <supple/notation/notation.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    auto text = std::string_view(reinterpret_cast<const char*>(data), size);
    auto result = supple::notation::compile(text);
    (void)result;
    return 0;
}
