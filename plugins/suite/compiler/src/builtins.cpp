// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The runtime's surface, declared once for the compiler. Each entry gives the
// declaration as script.h spells it and, for a thunk, the C++ function that
// calls the runtime; the declaration's types are checked against the thunk's
// own when the table is built, so a disagreement stops every test.
#include "builtins.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <set>
#include <type_traits>

#include "supple/script.h"
#include "supple/script_runtime.h"

namespace supple::scriptc {

namespace S = supple::script;

namespace {

template <class T>
constexpr Abi abiOf() {
    if constexpr (std::is_void_v<T>)
        return Abi::Void;
    else if constexpr (std::is_same_v<T, int>)
        return Abi::I32;
    else if constexpr (std::is_same_v<T, float>)
        return Abi::F32;
    else if constexpr (std::is_same_v<T, double>)
        return Abi::F64;
    else if constexpr (std::is_pointer_v<T>)
        return Abi::Ptr;
    else
        static_assert(!sizeof(T),
                      "a thunk takes and returns int, float, double or pointers (bool as int)");
}

template <class R, class... A>
std::vector<Abi> abiList(R (*)(A...)) {
    return {abiOf<R>(), abiOf<A>()...};
}

[[noreturn]] void fail(const std::string& what) {
    std::fprintf(stderr, "script compiler: bad built-in declaration: %s\n", what.c_str());
    std::abort();
}

}  // namespace

// Parses the declarations below ("double lerp(double a, double b, double t)")
// against the types it knows, and checks them against a thunk's ABI.
struct Registrar {
    Builtins& b;

    struct Decl {
        QualType result;
        std::string name;
        std::vector<QualType> params;
        bool isConst = false;
    };

    static bool ident(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':';
    }

    const Type* typeNamed(const std::string& n, const BuiltinClass* scope) const {
        if (n == "void") return b.void_;
        if (n == "bool") return b.bool_;
        if (n == "int") return b.int_;
        if (n == "float") return b.float_;
        if (n == "double") return b.double_;
        if (scope) {
            auto it = scope->nestedTypes.find(n);
            if (it != scope->nestedTypes.end()) return it->second;
        }
        if (auto* c = b.cls(n)) return c->type;
        if (auto* e = b.enumNamed(n)) return e;
        fail("unknown type " + n);
    }

    Decl parse(const std::string& text, const BuiltinClass* scope) const {
        Decl d;
        size_t i = 0;
        auto skip = [&] {
            while (i < text.size() && text[i] == ' ') ++i;
        };
        auto word = [&] {
            skip();
            size_t s = i;
            while (i < text.size() && ident(text[i])) ++i;
            return text.substr(s, i - s);
        };
        auto qual = [&](QualType& q) {
            std::string w = word();
            if (w == "const") {
                q.isConst = true;
                w = word();
            }
            q.type = typeNamed(w, scope);
            skip();
            if (i < text.size() && text[i] == '&') {
                q.isRef = true;
                ++i;
            }
        };
        qual(d.result);
        d.name = word();
        skip();
        if (i >= text.size() || text[i] != '(') fail(text);
        ++i;
        skip();
        while (i < text.size() && text[i] != ')') {
            QualType p;
            qual(p);
            word();  // the parameter's name, for documentation
            d.params.push_back(p);
            skip();
            if (i < text.size() && text[i] == ',') ++i;
            skip();
        }
        ++i;
        if (word() == "const") d.isConst = true;
        return d;
    }

    static Abi abiFor(const QualType& q) {
        if (q.isRef) return Abi::Ptr;
        switch (q.type->kind) {
            case TypeKind::Void:
                return Abi::Void;
            case TypeKind::Bool:
            case TypeKind::Int:
            case TypeKind::Enum:
                return Abi::I32;
            case TypeKind::Float:
                return Abi::F32;
            case TypeKind::Double:
                return Abi::F64;
            default:
                return Abi::Ptr;
        }
    }

    void check(const std::string& text, const std::vector<QualType>& params, const QualType& result,
               bool self, const std::vector<Abi>& abi) const {
        std::vector<Abi> want{abiFor(result)};
        if (self) want.push_back(Abi::Ptr);
        for (const auto& p : params) want.push_back(abiFor(p));
        if (want != abi) fail("the thunk's types do not match '" + text + "'");
    }

    template <class F>
    void fn(const std::string& text, F* thunk, std::uint8_t contexts = kInit | kAudio | kUi) {
        Decl d = parse(text, nullptr);
        auto f = std::make_unique<BuiltinFunction>();
        f->name = d.name;
        f->params = d.params;
        f->result = d.result;
        f->thunk = reinterpret_cast<void*>(thunk);
        f->abi = abiList(thunk);
        f->contexts = contexts;
        check(text, f->params, f->result, false, f->abi);
        b.byName_[f->name].push_back(f.get());
        b.functions_.push_back(std::move(f));
    }

    void intrinsic(const std::string& text, Intrinsic which) {
        Decl d = parse(text, nullptr);
        auto f = std::make_unique<BuiltinFunction>();
        f->name = d.name;
        f->params = d.params;
        f->result = d.result;
        f->intrinsic = which;
        b.byName_[f->name].push_back(f.get());
        b.functions_.push_back(std::move(f));
    }

    template <class F>
    void method(BuiltinClass* c, const std::string& text, F* thunk, Writes writes = Writes::Self,
                std::uint8_t contexts = kInit | kAudio | kUi) {
        Decl d = parse(text, c);
        BuiltinMethod m;
        m.owner = c;
        m.name = d.name;
        m.params = d.params;
        m.result = d.result;
        m.isConst = d.isConst;
        m.writes = d.isConst ? Writes::Nothing : writes;
        m.thunk = reinterpret_cast<void*>(thunk);
        m.abi = abiList(thunk);
        m.contexts = contexts;
        m.doc = text;
        check(text, m.params, m.result, true, m.abi);
        c->methods.push_back(std::move(m));
    }

    void inlineMethod(BuiltinClass* c, const std::string& text, Intrinsic which,
                      Writes writes = Writes::Nothing) {
        Decl d = parse(text, c);
        BuiltinMethod m;
        m.owner = c;
        m.name = d.name;
        m.params = d.params;
        m.result = d.result;
        m.isConst = d.isConst;
        m.writes = writes;
        m.intrinsic = which;
        m.doc = text;
        c->methods.push_back(std::move(m));
    }

    template <class T>
    void lifetime(BuiltinClass* c) {
        c->construct = +[](void* p) { new (p) T(); };
        c->destroy = +[](void* p) { static_cast<T*>(p)->~T(); };
        c->copy = +[](void* d, const void* s) { *static_cast<T*>(d) = *static_cast<const T*>(s); };
    }
};

Type* Builtins::scalar(TypeKind kind, const char* name, int size) {
    auto t = std::make_unique<Type>();
    t->kind = kind;
    t->name = name;
    t->size = size;
    t->align = size > 0 ? size : 1;
    types_.push_back(std::move(t));
    return types_.back().get();
}

BuiltinClass* Builtins::addClass(const std::string& name, int size, int align) {
    auto c = std::make_unique<BuiltinClass>();
    c->name = name;
    auto t = std::make_unique<Type>();
    t->kind = TypeKind::Object;
    t->name = name;
    t->size = size;
    t->align = align;
    t->builtin = c.get();
    c->type = t.get();
    types_.push_back(std::move(t));
    classByName_[name] = c.get();
    classes_.push_back(std::move(c));
    return classes_.back().get();
}

Builtins::Builtins() {
    void_ = scalar(TypeKind::Void, "void", 0);
    bool_ = scalar(TypeKind::Bool, "bool", 1);
    int_ = scalar(TypeKind::Int, "int", 4);
    float_ = scalar(TypeKind::Float, "float", 4);
    double_ = scalar(TypeKind::Double, "double", 8);
    string_ = scalar(TypeKind::String, "const char*", 8);
    initList_ = scalar(TypeKind::InitList, "{...}", 0);
    null_ = scalar(TypeKind::Null, "nullptr", 8);
    addClasses();
    addFunctions();
}

const Builtins& Builtins::get() {
    static const Builtins instance;
    return instance;
}

const BuiltinClass* Builtins::cls(const std::string& name) const {
    auto it = classByName_.find(name);
    return it == classByName_.end() ? nullptr : it->second;
}

const BuiltinEnum* Builtins::enumOf(const Type* type) const {
    for (const auto& e : enums_)
        if (e->type == type) return e.get();
    return nullptr;
}

const Type* Builtins::enumNamed(const std::string& name) const {
    auto it = enumByName_.find(name);
    return it == enumByName_.end() ? nullptr : it->second;
}

std::vector<const BuiltinFunction*> Builtins::functions(const std::string& name) const {
    auto it = byName_.find(name);
    return it == byName_.end() ? std::vector<const BuiltinFunction*>{} : it->second;
}

bool Builtins::reserved(const std::string& name) const {
    // The parameter factories, which Sema::evaluate knows by name.
    static const std::set<std::string> factories = {
        "continuous", "decibels",  "frequency", "milliseconds", "percent",
        "ratio",      "semitones", "cents",     "octaves",      "degrees",
        "integer",    "choice",    "toggle",    "audioFile",    "impulseResponse"};
    return byName_.count(name) || classByName_.count(name) || enumByName_.count(name) ||
           factories.count(name) || name == "pi" || name == "twoPi" || name == "Array" ||
           name == "Buffer" || name == "Unit" || name == "Curve";
}

bool Builtins::isFile(const Type* t) const {
    return t && t->kind == TypeKind::Object &&
           (t->builtin == audioFile_ || t->builtin == convolver_);
}

int Builtins::pluginBaseSize() const { return static_cast<int>(sizeof(S::Plugin)); }

void Builtins::addClasses() {
    Registrar r{*this};
    using L = S::detail::Layout;

    // Plain value views and records the runtime hands a script.
    auto* transport = addClass("Transport", sizeof(S::Transport), alignof(S::Transport));
    transport->fields = {
        {"playing", bool_, static_cast<int>(offsetof(S::Transport, playing))},
        {"tempo", double_, static_cast<int>(offsetof(S::Transport, tempo))},
        {"beats", double_, static_cast<int>(offsetof(S::Transport, beats))},
        {"barStart", double_, static_cast<int>(offsetof(S::Transport, barStart))},
        {"timeSigNumerator", int_, static_cast<int>(offsetof(S::Transport, timeSigNumerator))},
        {"timeSigDenominator", int_, static_cast<int>(offsetof(S::Transport, timeSigDenominator))}};
    transport->declarable = false;
    transport_ = transport;

    auto* channel = addClass("Channel", sizeof(S::Channel), alignof(S::Channel));
    channel->declarable = false;
    channel_ = channel;
    auto* input = addClass("InputChannel", sizeof(S::InputChannel), alignof(S::InputChannel));
    input->declarable = false;
    inputChannel_ = input;
    r.inlineMethod(channel, "int size() const", Intrinsic::None);
    r.inlineMethod(input, "int size() const", Intrinsic::None);

    auto* block = addClass("Block", sizeof(S::Block), alignof(S::Block));
    block->declarable = false;
    block->fields = {
        {"channels", int_, static_cast<int>(offsetof(S::Block, channels))},
        {"frames", int_, static_cast<int>(offsetof(S::Block, frames))},
        {"transport", transport->type, static_cast<int>(offsetof(S::Block, transport))},
        {"sidechainConnected", bool_, static_cast<int>(offsetof(S::Block, sidechainConnected))}};
    r.inlineMethod(block, "InputChannel sidechain(int c) const", Intrinsic::BlockSidechain);
    r.inlineMethod(block, "Channel output(int bus, int c) const", Intrinsic::BlockOutput);
    block_ = block;

    auto* param = addClass("Param", sizeof(S::Param), alignof(S::Param));
    param->declarable = true;  // as a member of the plugin struct only (sema checks)
    r.inlineMethod(param, "double value() const", Intrinsic::ParamValue);
    r.inlineMethod(param, "double at(int i) const", Intrinsic::ParamAt);
    r.inlineMethod(param, "int index() const", Intrinsic::ParamIndex);
    r.inlineMethod(param, "bool on() const", Intrinsic::ParamOn);
    r.inlineMethod(param, "bool changed() const", Intrinsic::ParamChanged);
    r.inlineMethod(param, "void set(double value)", Intrinsic::ParamSet, Writes::Self);
    r.method(param, "double gain() const", +[](const S::Param* p) { return p->gain(); });
    param_ = param;
    (void)L::paramValue;

    // The compile-time records: never values at run time.
    auto* spec = addClass("ParamSpec", sizeof(S::ParamSpec), alignof(S::ParamSpec));
    spec->declarable = false;
    paramSpec_ = spec;
    auto* info = addClass("Info", sizeof(S::Info), alignof(S::Info));
    info->declarable = false;
    info_ = info;

    // VoiceMode, at the top level.
    {
        auto mode = std::make_unique<BuiltinEnum>();
        auto t = std::make_unique<Type>();
        t->kind = TypeKind::Enum;
        t->name = "VoiceMode";
        t->size = 4;
        t->align = 4;
        mode->type = t.get();
        types_.push_back(std::move(t));
        mode->values = {{"Poly", 0}, {"Mono", 1}, {"Legato", 2}};
        static_assert(static_cast<int>(S::VoiceMode::Legato) == 2);
        enumByName_["VoiceMode"] = mode->type;
        enums_.push_back(std::move(mode));
    }

    auto* plugin = addClass("Plugin", sizeof(S::Plugin), alignof(S::Plugin));
    plugin->declarable = false;
    r.inlineMethod(plugin, "double sampleRate() const", Intrinsic::SampleRate);
    r.inlineMethod(plugin, "int maxBlockSize() const", Intrinsic::MaxBlockSize);
    r.inlineMethod(plugin, "int channels() const", Intrinsic::Channels);
    r.inlineMethod(plugin, "bool offline() const", Intrinsic::Offline);
    // An instrument's (sema allows them in instruments only): the plugin
    // struct is an Instrument, whose base is at its start.
    r.method(
        plugin, "int voiceCount() const", +[](const S::Instrument* p) { return p->voiceCount(); });
    r.method(
        plugin, "bool voicePlaying(int v) const",
        +[](const S::Instrument* p, int v) -> int { return p->voicePlaying(v); });
    r.method(
        plugin, "int voiceKey(int v) const",
        +[](const S::Instrument* p, int v) { return p->voiceKey(v); });
    r.method(
        plugin, "int voiceChannel(int v) const",
        +[](const S::Instrument* p, int v) { return p->voiceChannel(v); });
    r.method(
        plugin, "double voiceVelocity(int v) const",
        +[](const S::Instrument* p, int v) { return p->voiceVelocity(v); });
    r.method(
        plugin, "double voicePitch(int v, int i) const",
        +[](const S::Instrument* p, int v, int i) { return p->voicePitch(v, i); });
    r.method(
        plugin, "double voiceHz(int v, int i) const",
        +[](const S::Instrument* p, int v, int i) { return p->voiceHz(v, i); });
    r.method(
        plugin, "void voiceFinished(int v)", +[](S::Instrument* p, int v) { p->voiceFinished(v); });
    r.method(
        plugin, "void setVoiceMode(VoiceMode mode)",
        +[](S::Instrument* p, int m) { p->setVoiceMode(static_cast<S::VoiceMode>(m)); });
    r.method(
        plugin, "void setGlide(double ms)", +[](S::Instrument* p, double ms) { p->setGlide(ms); });
    r.method(
        plugin, "void setBendRange(double semitones)",
        +[](S::Instrument* p, double st) { p->setBendRange(st); });
    for (BuiltinMethod& m : plugin->methods) m.instrument = m.intrinsic == Intrinsic::None;
    // Every plugin's, in prepare().
    r.method(
        plugin, "void setLatency(int samples)", +[](S::Plugin* p, int n) { p->setLatency(n); },
        Writes::Self, kInit);
    plugin_ = plugin;

    // DSP objects.
    auto* biquad = addClass("Biquad", sizeof(S::Biquad), alignof(S::Biquad));
    r.lifetime<S::Biquad>(biquad);
    r.method(
        biquad, "void lowpass(double hz, double q)",
        +[](S::Biquad* f, double hz, double q) { f->lowpass(hz, q); });
    r.method(
        biquad, "void highpass(double hz, double q)",
        +[](S::Biquad* f, double hz, double q) { f->highpass(hz, q); });
    r.method(
        biquad, "void bandpass(double hz, double q)",
        +[](S::Biquad* f, double hz, double q) { f->bandpass(hz, q); });
    r.method(
        biquad, "void notch(double hz, double q)",
        +[](S::Biquad* f, double hz, double q) { f->notch(hz, q); });
    r.method(
        biquad, "void allpass(double hz, double q)",
        +[](S::Biquad* f, double hz, double q) { f->allpass(hz, q); });
    r.method(
        biquad, "void peak(double hz, double q, double gainDb)",
        +[](S::Biquad* f, double hz, double q, double g) { f->peak(hz, q, g); });
    r.method(
        biquad, "void lowShelf(double hz, double q, double gainDb)",
        +[](S::Biquad* f, double hz, double q, double g) { f->lowShelf(hz, q, g); });
    r.method(
        biquad, "void highShelf(double hz, double q, double gainDb)",
        +[](S::Biquad* f, double hz, double q, double g) { f->highShelf(hz, q, g); });
    r.method(
        biquad, "double process(double x)", +[](S::Biquad* f, double x) { return f->process(x); });
    r.method(biquad, "void clear()", +[](S::Biquad* f) { f->clear(); });
    r.method(
        biquad, "double responseDb(double hz) const",
        +[](const S::Biquad* f, double hz) { return f->responseDb(hz); });

    auto* onePole = addClass("OnePole", sizeof(S::OnePole), alignof(S::OnePole));
    r.lifetime<S::OnePole>(onePole);
    r.method(
        onePole, "void setCutoff(double hz)", +[](S::OnePole* f, double hz) { f->setCutoff(hz); });
    r.method(
        onePole, "double lowpass(double x)",
        +[](S::OnePole* f, double x) { return f->lowpass(x); });
    r.method(
        onePole, "double highpass(double x)",
        +[](S::OnePole* f, double x) { return f->highpass(x); });
    r.method(onePole, "void clear()", +[](S::OnePole* f) { f->clear(); });

    auto* follower =
        addClass("EnvelopeFollower", sizeof(S::EnvelopeFollower), alignof(S::EnvelopeFollower));
    r.lifetime<S::EnvelopeFollower>(follower);
    r.method(
        follower, "void set(double attackMs, double releaseMs)",
        +[](S::EnvelopeFollower* f, double a, double r) { f->set(a, r); });
    r.method(
        follower, "double process(double x)",
        +[](S::EnvelopeFollower* f, double x) { return f->process(x); });
    r.method(
        follower, "double level() const", +[](const S::EnvelopeFollower* f) { return f->level(); });
    r.method(follower, "void clear()", +[](S::EnvelopeFollower* f) { f->clear(); });

    // File members: a pointer the glue sets (no construction; copied as bytes).
    auto* file = addClass("AudioFile", sizeof(S::AudioFile), alignof(S::AudioFile));
    audioFile_ = file;
    r.method(
        file, "bool loaded() const", +[](const S::AudioFile* f) -> int { return f->loaded(); });
    r.method(file, "int channels() const", +[](const S::AudioFile* f) { return f->channels(); });
    r.method(file, "int frames() const", +[](const S::AudioFile* f) { return f->frames(); });
    r.method(
        file, "double sample(int channel, int frame) const",
        +[](const S::AudioFile* f, int c, int i) { return f->sample(c, i); });
    r.method(
        file, "double read(int channel, double frame) const",
        +[](const S::AudioFile* f, int c, double at) { return f->read(c, at); });
    auto* convolver = addClass("Convolver", sizeof(S::Convolver), alignof(S::Convolver));
    convolver_ = convolver;
    r.method(
        convolver, "bool loaded() const",
        +[](const S::Convolver* f) -> int { return f->loaded(); });
    r.method(convolver, "int length() const", +[](const S::Convolver* f) { return f->length(); });
    r.method(
        convolver, "double process(int channel, double x)",
        +[](S::Convolver* f, int c, double x) { return f->process(c, x); });

    auto* delay = addClass("DelayLine", sizeof(S::DelayLine), alignof(S::DelayLine));
    delay->resource = true;
    r.lifetime<S::DelayLine>(delay);
    r.method(
        delay, "void prepare(int maxDelaySamples)", +[](S::DelayLine* d, int n) { d->prepare(n); },
        Writes::Self, kInit);
    r.method(delay, "void push(double x)", +[](S::DelayLine* d, double x) { d->push(x); });
    r.method(
        delay, "double read(double delaySamples) const",
        +[](const S::DelayLine* d, double samples) { return d->read(samples); });
    r.method(delay, "void clear()", +[](S::DelayLine* d) { d->clear(); });

    auto* over = addClass("Oversampler", sizeof(S::Oversampler), alignof(S::Oversampler));
    over->resource = true;
    r.lifetime<S::Oversampler>(over);
    r.method(
        over, "void prepare(int factor)", +[](S::Oversampler* o, int f) { o->prepare(f); },
        Writes::Self, kInit);
    r.method(over, "int factor() const", +[](const S::Oversampler* o) { return o->factor(); });
    r.method(over, "int latency() const", +[](const S::Oversampler* o) { return o->latency(); });
    r.method(over, "void up(double x)", +[](S::Oversampler* o, double x) { o->up(x); });
    r.method(
        over, "double sample(int k) const",
        +[](const S::Oversampler* o, int k) { return o->sample(k); });
    r.method(
        over, "void set(int k, double value)",
        +[](S::Oversampler* o, int k, double v) { o->set(k, v); });
    r.method(over, "double down()", +[](S::Oversampler* o) { return o->down(); });
    r.method(over, "void clear()", +[](S::Oversampler* o) { o->clear(); });

    auto* stft = addClass("Stft", sizeof(S::Stft), alignof(S::Stft));
    stft->resource = true;
    r.lifetime<S::Stft>(stft);
    r.method(
        stft, "void prepare(int size, int overlap)",
        +[](S::Stft* s, int size, int overlap) { s->prepare(size, overlap); }, Writes::Self, kInit);
    r.method(stft, "int size() const", +[](const S::Stft* s) { return s->size(); });
    r.method(stft, "int bins() const", +[](const S::Stft* s) { return s->bins(); });
    r.method(stft, "int hop() const", +[](const S::Stft* s) { return s->hop(); });
    r.method(stft, "int latency() const", +[](const S::Stft* s) { return s->latency(); });
    r.method(
        stft, "double binHz(int k) const", +[](const S::Stft* s, int k) { return s->binHz(k); });
    r.method(stft, "bool push(double x)", +[](S::Stft* s, double x) -> int { return s->push(x); });
    r.method(stft, "double pull()", +[](S::Stft* s) { return s->pull(); });
    r.method(stft, "double real(int k) const", +[](const S::Stft* s, int k) { return s->real(k); });
    r.method(stft, "double imag(int k) const", +[](const S::Stft* s, int k) { return s->imag(k); });
    r.method(
        stft, "double magnitude(int k) const",
        +[](const S::Stft* s, int k) { return s->magnitude(k); });
    r.method(
        stft, "double phase(int k) const", +[](const S::Stft* s, int k) { return s->phase(k); });
    r.method(
        stft, "void set(int k, double real, double imag)",
        +[](S::Stft* s, int k, double re, double im) { s->set(k, re, im); });
    r.method(
        stft, "void setPolar(int k, double magnitude, double phase)",
        +[](S::Stft* s, int k, double m, double p) { s->setPolar(k, m, p); });
    r.method(
        stft, "void scale(int k, double gain)",
        +[](S::Stft* s, int k, double g) { s->scale(k, g); });
    r.method(stft, "void clear()", +[](S::Stft* s) { s->clear(); });

    auto* random = addClass("Random", sizeof(S::Random), alignof(S::Random));
    r.lifetime<S::Random>(random);
    r.method(random, "void seed(int seed)", +[](S::Random* g, int s) { g->seed(s); });
    r.method(random, "double next()", +[](S::Random* g) { return g->next(); });
    r.method(random, "double bipolar()", +[](S::Random* g) { return g->bipolar(); });
    r.method(random, "double gaussian()", +[](S::Random* g) { return g->gaussian(); });
    r.method(random, "int below(int n)", +[](S::Random* g, int n) { return g->below(n); });

    auto* noise = addClass("Noise", sizeof(S::Noise), alignof(S::Noise));
    r.lifetime<S::Noise>(noise);
    r.method(noise, "void seed(int seed)", +[](S::Noise* g, int s) { g->seed(s); });
    r.method(noise, "double white()", +[](S::Noise* g) { return g->white(); });
    r.method(noise, "double pink()", +[](S::Noise* g) { return g->pink(); });
    r.method(noise, "double brown()", +[](S::Noise* g) { return g->brown(); });

    // Oscillator::Shape.
    auto shape = std::make_unique<BuiltinEnum>();
    {
        auto t = std::make_unique<Type>();
        t->kind = TypeKind::Enum;
        t->name = "Oscillator::Shape";
        t->size = 4;
        t->align = 4;
        shape->type = t.get();
        types_.push_back(std::move(t));
        shape->values = {{"Sine", 0}, {"Saw", 1}, {"Square", 2}, {"Triangle", 3}};
        static_assert(static_cast<int>(S::Oscillator::Shape::Triangle) == 3);
    }
    auto* osc = addClass("Oscillator", sizeof(S::Oscillator), alignof(S::Oscillator));
    osc->nestedTypes["Shape"] = shape->type;
    r.lifetime<S::Oscillator>(osc);
    r.method(
        osc, "void setShape(Shape shape)",
        +[](S::Oscillator* o, int s) { o->setShape(static_cast<S::Oscillator::Shape>(s)); });
    r.method(
        osc, "void setFrequency(double hz)",
        +[](S::Oscillator* o, double hz) { o->setFrequency(hz); });
    r.method(
        osc, "void setPulseWidth(double width)",
        +[](S::Oscillator* o, double w) { o->setPulseWidth(w); });
    r.method(
        osc, "void setPhase(double phase)", +[](S::Oscillator* o, double p) { o->setPhase(p); });
    r.method(osc, "double phase() const", +[](const S::Oscillator* o) { return o->phase(); });
    r.method(osc, "double next()", +[](S::Oscillator* o) { return o->next(); });
    enums_.push_back(std::move(shape));

    auto* adsr = addClass("Adsr", sizeof(S::Adsr), alignof(S::Adsr));
    r.lifetime<S::Adsr>(adsr);
    r.method(
        adsr, "void set(double attackMs, double decayMs, double sustain, double releaseMs)",
        +[](S::Adsr* e, double a, double d, double s, double rl) { e->set(a, d, s, rl); });
    r.method(
        adsr, "void setCurves(double attack, double decayRelease)",
        +[](S::Adsr* e, double a, double d) { e->setCurves(a, d); });
    r.method(adsr, "void gate(bool on)", +[](S::Adsr* e, int on) { e->gate(on != 0); });
    r.method(adsr, "void retrigger()", +[](S::Adsr* e) { e->retrigger(); });
    r.method(adsr, "void kill()", +[](S::Adsr* e) { e->kill(); });
    r.method(adsr, "double next()", +[](S::Adsr* e) { return e->next(); });
    r.method(adsr, "double level() const", +[](const S::Adsr* e) { return e->level(); });
    r.method(adsr, "bool active() const", +[](const S::Adsr* e) -> int { return e->active(); });
}

void Builtins::addFunctions() {
    Registrar r{*this};
    // Math: the runtime's own functions, one compiled copy for every path.
    r.fn("double sin(double x)", +[](double x) { return S::sin(x); });
    r.fn("double cos(double x)", +[](double x) { return S::cos(x); });
    r.fn("double tan(double x)", +[](double x) { return S::tan(x); });
    r.fn("double asin(double x)", +[](double x) { return S::asin(x); });
    r.fn("double acos(double x)", +[](double x) { return S::acos(x); });
    r.fn("double atan(double x)", +[](double x) { return S::atan(x); });
    r.fn("double atan2(double y, double x)", +[](double y, double x) { return S::atan2(y, x); });
    r.fn("double sinh(double x)", +[](double x) { return S::sinh(x); });
    r.fn("double cosh(double x)", +[](double x) { return S::cosh(x); });
    r.fn("double tanh(double x)", +[](double x) { return S::tanh(x); });
    r.fn("double exp(double x)", +[](double x) { return S::exp(x); });
    r.fn("double exp2(double x)", +[](double x) { return S::exp2(x); });
    r.fn("double log(double x)", +[](double x) { return S::log(x); });
    r.fn("double log2(double x)", +[](double x) { return S::log2(x); });
    r.fn("double log10(double x)", +[](double x) { return S::log10(x); });
    r.fn("double pow(double x, double y)", +[](double x, double y) { return S::pow(x, y); });
    r.fn("double sqrt(double x)", +[](double x) { return S::sqrt(x); });
    r.fn("double cbrt(double x)", +[](double x) { return S::cbrt(x); });
    r.fn("double floor(double x)", +[](double x) { return S::floor(x); });
    r.fn("double ceil(double x)", +[](double x) { return S::ceil(x); });
    r.fn("double trunc(double x)", +[](double x) { return S::trunc(x); });
    r.fn("double round(double x)", +[](double x) { return S::round(x); });
    r.fn("double fmod(double x, double y)", +[](double x, double y) { return S::fmod(x, y); });
    r.fn("double hypot(double x, double y)", +[](double x, double y) { return S::hypot(x, y); });

    r.intrinsic("double abs(double x)", Intrinsic::Abs);
    r.intrinsic("int abs(int x)", Intrinsic::Abs);
    r.intrinsic("double min(double a, double b)", Intrinsic::Min);
    r.intrinsic("int min(int a, int b)", Intrinsic::Min);
    r.intrinsic("double max(double a, double b)", Intrinsic::Max);
    r.intrinsic("int max(int a, int b)", Intrinsic::Max);
    r.intrinsic("double clamp(double x, double lo, double hi)", Intrinsic::Clamp);
    r.intrinsic("int clamp(int x, int lo, int hi)", Intrinsic::Clamp);
    r.intrinsic("double lerp(double a, double b, double t)", Intrinsic::Lerp);
    r.intrinsic("double sign(double x)", Intrinsic::Sign);
    r.intrinsic("bool isFinite(double x)", Intrinsic::IsFinite);

    r.fn("double dbToGain(double db)", +[](double x) { return S::dbToGain(x); });
    r.fn("double gainToDb(double gain)", +[](double x) { return S::gainToDb(x); });
    r.fn("double mtof(double note)", +[](double x) { return S::mtof(x); });
    r.fn("double ftom(double hz)", +[](double x) { return S::ftom(x); });
    r.fn(
        "double compressorGainDb(double levelDb, double thresholdDb, double ratio, double kneeDb)",
        +[](double l, double t, double r, double k) { return S::compressorGainDb(l, t, r, k); });
    r.fn("int toInt(double x)", +[](double x) { return S::toInt(x); });
    r.fn("int floorToInt(double x)", +[](double x) { return S::floorToInt(x); });
    r.fn("int ceilToInt(double x)", +[](double x) { return S::ceilToInt(x); });
    r.fn("int roundToInt(double x)", +[](double x) { return S::roundToInt(x); });
    r.fn("int idiv(int a, int b)", +[](int a, int b) { return S::idiv(a, b); });
    r.fn("int imod(int a, int b)", +[](int a, int b) { return S::imod(a, b); });
    r.fn("int wrap(int i, int n)", +[](int i, int n) { return S::wrap(i, n); });
}

}  // namespace supple::scriptc
