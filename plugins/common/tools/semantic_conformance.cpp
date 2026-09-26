// semantic-conformance <plugin.clap> <plugin-id>
//
// Checks a plugin's com.suppleaudio.semantic-params/1 implementation against
// docs/specs/semantic-params-extension.md §7 through the real CLAP host.
// Every stock plugin registers it as a CTest test. Exit 0 = conforming.
#include <cstdio>
#include <string>
#include <vector>

#include "supple/clap/semantic_params.h"
#include "supple/host/clap_host.h"
#include "supple/json/json.h"

using namespace supple;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string get(host::Instance& in, bool changed) {
    auto p = in.getPatch(changed);
    return p ? *p : std::string("<error: ") + p.error().toString() + ">";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <plugin.clap> <plugin-id>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path path = argv[1];
    const std::string id = argv[2];
    auto list = host::scan(std::span<const std::filesystem::path>(&path, 1));
    if (!list) {
        std::fprintf(stderr, "scan failed: %s\n", list.error().toString().c_str());
        return 2;
    }
    const host::PluginInfo* info = nullptr;
    for (const auto& p : *list)
        if (p.id == id) info = &p;
    if (!info) {
        std::fprintf(stderr, "plugin %s not found in %s\n", id.c_str(), path.c_str());
        return 2;
    }
    check(info->semantic, "implements " + std::string(CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    if (!info->semantic) return 1;

    auto made = host::Instance::create(*info, {});
    if (!made) {
        std::fprintf(stderr, "create failed: %s\n", made.error().toString().c_str());
        return 2;
    }
    host::Instance& in = **made;

    // 1. Schema parses and matches clap.params.
    auto schema = in.semanticSchema();
    check(schema.ok(), "schema is valid JSON");
    if (!schema) return 1;
    const json::Value* params = schema->find("params");
    check(params && params->isObject(), "schema has a params object");
    check(schema->find("patch_type") && schema->find("patch_type")->isString(),
          "schema names its patch type");
    const auto clapParams = in.params();
    std::size_t matched = 0;
    std::string firstFloat;
    double floatMin = 0, floatMax = 1;
    if (params && params->isObject()) {
        for (const auto& m : params->asObject()) {
            const json::Value& e = m.value;
            const auto pid = e.find("id") ? e.find("id")->asInt() : -1;
            const host::ParamInfo* cp = nullptr;
            for (const auto& c : clapParams)
                if (static_cast<std::int64_t>(c.id) == pid) cp = &c;
            if (!cp) {
                check(false, "schema param " + m.key + " exists in clap.params");
                continue;
            }
            ++matched;
            const std::string type = e.find("type") ? e.find("type")->asString() : "";
            const bool stepped = (cp->flags & CLAP_PARAM_IS_STEPPED) != 0;
            if (type == "float") {
                check(cp->min == 0.0 && cp->max == 1.0 && !stepped,
                      m.key + ": float params use the normalized CLAP range (ADR-0010)");
                const bool readonly =
                    e.find("flags") &&
                    json::writeCompact(*e.find("flags")).find("readonly") != std::string::npos;
                if (firstFloat.empty() && !readonly) {
                    firstFloat = m.key;
                    floatMin = e.find("min")->asDouble();
                    floatMax = e.find("max")->asDouble();
                }
            } else {
                check(stepped, m.key + ": " + type + " params are stepped");
            }
            check(e.find("name") && e.find("name")->asString() == cp->name,
                  m.key + ": name matches clap.params");
        }
    }
    check(matched == clapParams.size(), "every clap.params entry is described (" +
                                            std::to_string(matched) + "/" +
                                            std::to_string(clapParams.size()) + ")");

    // 2. get_patch -> set_patch(REPLACE) is the identity.
    const std::string full = get(in, false);
    check(in.setPatch(full, CLAP_SUPPLE_SET_REPLACE).ok(), "full patch re-applies");
    check(get(in, false) == full, "get/set round trip is the identity");

    // 3. The CHANGED detail is a complete patch describing the same state.
    const std::string changed = get(in, true);
    check(in.setPatch(changed, CLAP_SUPPLE_SET_REPLACE).ok() && get(in, false) == full,
          "changed-detail patch reproduces the same state");

    // 4. Merging one value changes only that value.
    if (!firstFloat.empty()) {
        const double target = floatMin + (floatMax - floatMin) * 0.37;
        json::Value merge = json::Value::object();
        merge["params"][firstFloat] = target;
        const auto before = json::parse(full);
        check(in.setPatch(json::writeCompact(merge), CLAP_SUPPLE_SET_MERGE).ok(),
              "single-value merge applies");
        const auto after = json::parse(get(in, false));
        bool onlyThat = before && after;
        if (onlyThat) {
            for (const auto& ch : json::diff(*before, *after))
                onlyThat = onlyThat && ch.path == "/params/" + json::pointerEscape(firstFloat);
        }
        check(onlyThat, "merge changed only " + firstFloat);
        check(in.setPatch(full, CLAP_SUPPLE_SET_REPLACE).ok(), "restore");
    }

    // 5. Invalid documents fail with paths and change nothing.
    const std::string snapshot = get(in, false);
    auto bad = in.setPatch(R"({"params": {"no_such_param_xyz": 1}})", CLAP_SUPPLE_SET_MERGE);
    check(!bad.ok(), "unknown parameter is rejected");
    if (!bad.ok())
        check(bad.error().path == "/params/no_such_param_xyz", "error carries a JSON Pointer path");
    auto junk = in.setPatch("{not json", CLAP_SUPPLE_SET_MERGE);
    check(!junk.ok(), "malformed JSON is rejected");
    check(get(in, false) == snapshot, "rejected patches leave the patch unchanged");

    // 6. VALIDATE_ONLY never changes state.
    if (!firstFloat.empty()) {
        json::Value merge = json::Value::object();
        merge["params"][firstFloat] = floatMax;
        check(in.setPatch(json::writeCompact(merge), CLAP_SUPPLE_SET_VALIDATE_ONLY).ok(),
              "validate-only accepts");
        check(get(in, false) == snapshot, "validate-only changes nothing");
    }

    // 8. State save/load equals the patch round trip.
    auto state = in.saveState();
    check(state.ok(), "state saves");
    auto other = host::Instance::create(*info, {});
    if (state && other) {
        check((*other)->loadState(*state).ok(), "state loads into a fresh instance");
        check(get(**other, false) == snapshot, "loaded state has the same patch");
    }

    // 9. describe() is useful and short.
    auto d = in.describe(true);
    check(d && !d->empty() && d->size() <= 600, "describe(changed) is non-empty and <= 600 chars");

    std::printf("%s: %d failure(s)\n", id.c_str(), failures);
    return failures ? 1 : 0;
}
