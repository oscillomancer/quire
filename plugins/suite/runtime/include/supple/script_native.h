// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A script compiled as C++ (a source export), as the glue's Code
// (supple/script_code.h): the glue runs it as it runs the scripts the player
// compiles, and describes the same plugin.
//
//   namespace supple::script::user {
//   #include "script/tilt.cpp"
//   }
//   using Tilt = supple::script::user::Tilt;
//   const NativeCode<Tilt> code({{&Tilt::tilt, "tilt"}, {&Tilt::pivot, "pivot"}});
//
// The description comes from the script itself: Info from `info`, each
// Param's spec from its constructor, in declaration order. What C++ cannot
// see is a member's name, which a parameter's patch key is made from: the
// keys come with the code, by member (what the script compiler worked out).
// A Param with no key there takes the one withKey() gives, or else its
// name's in snake_case. Scripts never see this header.
#pragma once

#include <cctype>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "supple/script.h"
#include "supple/script_code.h"
#include "supple/script_runtime.h"

namespace supple::script::detail {

template <class T>
class NativeCode final : public Code {
  public:
    using Key = std::pair<Param T::*, const char*>;

    explicit NativeCode(std::vector<Key> keys = {}) {
        static_assert(std::is_base_of_v<Plugin, T>, "a plugin struct derives from Effect");
        describe(T::info);
        kind_ = std::is_base_of_v<Instrument, T>   ? PluginKind::Instrument
                : std::is_base_of_v<NoteEffect, T> ? PluginKind::NoteEffect
                                                   : PluginKind::Effect;
        // One instance, made to watch its parameters declare themselves.
        Collector collector;
        setRegistrar(&collector);
        auto probe = std::make_unique<T>();
        setRegistrar(nullptr);
        const auto* base = reinterpret_cast<const unsigned char*>(probe.get());
        for (size_t i = 0; i < collector.params.size(); ++i) {
            ParamModel& m = collector.models[i];
            const Param* param = collector.params[i];
            m.offset = static_cast<int>(reinterpret_cast<const unsigned char*>(param) - base);
            for (const Key& k : keys)
                if (&((*probe).*(k.first)) == param && m.key.empty()) m.key = k.second;
            if (m.key.empty()) m.key = snakeCase(m.name);
            if (m.id == 0) m.id = keyId(m.key);
            params_.push_back(std::move(m));
        }
    }

    const InfoModel& info() const override { return info_; }
    PluginKind kind() const override { return kind_; }
    const std::vector<ParamModel>& params() const override { return params_; }

    void* create() const override { return new T(); }
    void destroy(void* self) const override { delete static_cast<T*>(self); }
    void copy(void* dst, const void* src) const override {
        *static_cast<T*>(dst) = *static_cast<const T*>(src);
    }

    Hook prepare() const override {
        if constexpr (requires(T& t) { t.prepare(); })
            return [](void*, void* self) { static_cast<T*>(self)->prepare(); };
        return nullptr;
    }
    Hook update() const override {
        if constexpr (requires(T& t) { t.update(); })
            return [](void*, void* self) { static_cast<T*>(self)->update(); };
        return nullptr;
    }
    ProcessHook process() const override {
        if constexpr (requires(T& t, Block& b) { t.process(b); })
            return [](void*, void* self, void* block) {
                static_cast<T*>(self)->process(*static_cast<Block*>(block));
            };
        return nullptr;
    }
    VoiceHook startVoice() const override {
        if constexpr (requires(T& t) { t.startVoice(0); })
            return [](void*, void* self, std::int64_t v) {
                static_cast<T*>(self)->startVoice(static_cast<int>(v));
            };
        return nullptr;
    }
    StartHook startVoiceRetrigger() const override {
        if constexpr (requires(T& t) { t.startVoice(0, true); })
            return [](void*, void* self, std::int64_t v, std::int64_t retrigger) {
                static_cast<T*>(self)->startVoice(static_cast<int>(v), retrigger != 0);
            };
        return nullptr;
    }
    VoiceHook releaseVoice() const override {
        if constexpr (requires(T& t) { t.releaseVoice(0); })
            return [](void*, void* self, std::int64_t v) {
                static_cast<T*>(self)->releaseVoice(static_cast<int>(v));
            };
        return nullptr;
    }
    VoiceHook stopVoice() const override {
        if constexpr (requires(T& t) { t.stopVoice(0); })
            return [](void*, void* self, std::int64_t v) {
                static_cast<T*>(self)->stopVoice(static_cast<int>(v));
            };
        return nullptr;
    }

  private:
    struct Collector final : ParamRegistrar {
        std::vector<const Param*> params;
        std::vector<ParamModel> models;
        void declare(Param& param, const ParamSpec& spec) override {
            params.push_back(&param);
            models.push_back(model(spec));
        }
    };

    // What the script compiler makes of the same spec. The enums match
    // script.h's, value for value.
    static_assert(static_cast<int>(ParamSpec::Kind::Toggle) ==
                  static_cast<int>(ParamModel::Kind::Toggle));
    static_assert(static_cast<int>(Unit::Samples) == static_cast<int>(ParamModel::Unit::Samples));
    static_assert(static_cast<int>(Unit::Bpm) == static_cast<int>(ParamModel::Unit::Bpm));
    static_assert(static_cast<int>(Curve::Pow) == static_cast<int>(ParamModel::Curve::Pow));
    static ParamModel model(const ParamSpec& s) {
        ParamModel m;
        m.key = s.key;
        m.id = s.id;
        m.kind = static_cast<ParamModel::Kind>(s.kind);
        m.name = s.name;
        m.min = s.min;
        m.max = s.max;
        m.def = s.def;
        m.unit = static_cast<ParamModel::Unit>(s.unit);
        m.curve = static_cast<ParamModel::Curve>(s.curve);
        m.curveK = s.curveK;
        for (const char* c : s.choices) m.choices.emplace_back(c);
        m.group = s.group;
        m.description = s.description;
        m.smoothingMs = s.smoothingMs;
        m.automatable = s.automatable;
        m.modulatable = s.modulatable;
        m.offAtMinimum = s.offAtMinimum;
        m.output = s.output;
        m.bypass = s.bypass;
        m.hidden = s.hidden;
        for (const char* r : s.roles) m.roles.emplace_back(r);
        return m;
    }

    void describe(const Info& i) {
        info_.name = i.name;
        info_.id = i.id;
        info_.vendor = i.vendor;
        info_.version = i.version;
        info_.description = i.description;
        info_.url = i.url;
        for (int w : i.channels)
            if (w > 0) info_.channels.push_back(w);
        info_.sidechain = i.sidechain;
        for (const char* o : i.outputs)
            if (o && *o) info_.outputs.emplace_back(o);
        info_.voices = i.voices;
        info_.latency = i.latency;
        info_.tail = i.tail;
    }

    // A parameter's name as a key: "Low Cut" -> low_cut.
    static std::string snakeCase(const std::string& name) {
        std::string out;
        bool gap = false;
        for (char ch : name) {
            const auto c = static_cast<unsigned char>(ch);
            if (std::isalnum(c)) {
                if (gap && !out.empty()) out += '_';
                gap = false;
                out += static_cast<char>(std::tolower(c));
            } else {
                gap = true;
            }
        }
        if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0]))) out = "p" + out;
        return out;
    }

    InfoModel info_;
    PluginKind kind_ = PluginKind::Effect;
    std::vector<ParamModel> params_;
};

}  // namespace supple::script::detail
