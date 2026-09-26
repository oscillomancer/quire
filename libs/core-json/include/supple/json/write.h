#pragma once
#include <supple/base/result.h>
#include <supple/json/value.h>
namespace supple::json {
struct WriteOptions {
    bool pretty = true;
    bool sortKeys = false;
    int indent = 2;
    int maxLineWidth = 100;
};
Result<std::string> write(const Value& value, const WriteOptions& options = {});
std::string writeCompact(const Value& value);
std::string writeCanonical(const Value& value);
std::string formatDouble(double value);
}  // namespace supple::json
