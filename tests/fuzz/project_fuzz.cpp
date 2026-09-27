#include <supple/model/schemas.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > (1U << 20)) return 0;
    auto project =
        supple::model::parseProject(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!project) return 0;
    if (!supple::model::checkInvariants(*project)) __builtin_trap();
    const std::string text = supple::model::serializeProject(*project);
    auto again = supple::model::parseProject(text);
    if (!again || !supple::model::checkInvariants(*again) ||
        supple::model::serializeProject(*again) != text)
        __builtin_trap();
    return 0;
}
