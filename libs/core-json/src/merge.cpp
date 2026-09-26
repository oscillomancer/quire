#include <supple/json/merge.h>
#include <supple/json/pointer.h>
namespace supple::json {
Value mergePatch(const Value& target, const Value& patch) {
    if (!patch.isObject()) return patch;
    Value out = target.isObject() ? target : Value::object();
    for (const auto& m : patch.asObject()) {
        if (m.value.isNull())
            out.asObject().erase(m.key);
        else {
            const Value* old = out.find(m.key);
            out[m.key] = mergePatch(old ? *old : Value{}, m.value);
        }
    }
    return out;
}
Value createMergePatch(const Value& from, const Value& to) {
    if (!to.isObject()) return to;
    if (!from.isObject()) return to;
    Value patch = Value::object();
    for (const auto& m : from.asObject())
        if (!to.find(m.key)) patch[m.key] = nullptr;
    for (const auto& m : to.asObject()) {
        const Value* old = from.find(m.key);
        if (!old || !(*old == m.value))
            patch[m.key] = old ? createMergePatch(*old, m.value) : m.value;
    }
    return patch;
}
namespace {
void collect(const Value& a, const Value& b, std::string path, std::vector<Change>& out) {
    if (a == b) return;
    if (a.isObject() && b.isObject()) {
        for (const auto& m : a.asObject()) {
            const Value* next = b.find(m.key);
            if (!next)
                out.push_back({Change::Op::Remove, pointerJoin(path, m.key), m.value, {}});
            else
                collect(m.value, *next, pointerJoin(path, m.key), out);
        }
        for (const auto& m : b.asObject())
            if (!a.find(m.key))
                out.push_back({Change::Op::Add, pointerJoin(path, m.key), {}, m.value});
    } else if (a.isArray() && b.isArray() && a.size() == b.size()) {
        for (std::size_t i = 0; i < a.size(); ++i)
            collect(a.at(i), b.at(i), pointerJoin(path, i), out);
    } else
        out.push_back({Change::Op::Replace, std::move(path), a, b});
}
}  // namespace
std::vector<Change> diff(const Value& from, const Value& to) {
    std::vector<Change> out;
    collect(from, to, "", out);
    return out;
}
}  // namespace supple::json
