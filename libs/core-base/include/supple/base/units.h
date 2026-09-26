#pragma once
#include <supple/base/result.h>
#include <string_view>
namespace supple::units {
enum class Unit {
    Hz,
    ms,
    dB,
    LUFS,
    LU,
    st,
    ct,
    oct,
    percent,
    ratio,
    x,
    deg,
    bpm,
    beats,
    samples,
    none
};
std::string_view unitName(Unit unit);
std::string_view dimensionName(Unit unit);
Result<double> parseQuantity(std::string_view text, Unit fieldUnit);
std::string formatQuantity(double value, Unit unit, int significantDigits = 3);
}  // namespace supple::units
