// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The script runtime's implementation: the one copy of every built-in. A
// source export compiles this file; the JIT calls the same functions through
// the thunks in thunks.cpp. The DSP itself is the plugin framework's
// (framework/dsp); this file only gives it script-facing names and finds the
// sample rate.
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "dsp/Envelope.h"
#include "dsp/Filters.h"
#include "dsp/Interpolation.h"
#include "dsp/Math.h"
#include "dsp/Oscillators.h"
#include "supple/script.h"
#include "supple/script_runtime.h"

namespace supple::script {

namespace detail {

namespace {
// Which instance each thread is running a hook of: a fixed table of
// (thread, instance) pairs, not thread_local storage, which a dynamically
// loaded module gets on a thread's first touch -- an allocation on the
// audio thread's first block. A thread holds a slot only while it is inside
// a hook (CurrentScope), so the table stays small; it is scanned up to the
// highest slot ever taken.
constexpr int kSlots = 128;
struct Slot {
    std::atomic<std::uintptr_t> thread{0};
    Instance* instance = nullptr;  // only its thread reads or writes it
};
Slot gSlots[kSlots];
std::atomic<int> gHighest{0};

std::uintptr_t thisThread() {
#ifdef _WIN32
    return static_cast<std::uintptr_t>(GetCurrentThreadId()) + 1;
#else
    return static_cast<std::uintptr_t>(pthread_self()) | 1;  // never 0, the free mark
#endif
}

Slot* slotOf(std::uintptr_t me) {
    const int highest = gHighest.load(std::memory_order_acquire);
    for (int i = 0; i < highest; ++i)
        if (gSlots[i].thread.load(std::memory_order_relaxed) == me) return &gSlots[i];
    return nullptr;
}

thread_local ParamRegistrar* tRegistrar = nullptr;  // construction: never the audio thread
// Built-in objects used outside any instance (a test, a constant) work at
// this rate.
Instance gDefault;
}  // namespace

Instance* current() {
    const Slot* slot = slotOf(thisThread());
    return slot ? slot->instance : nullptr;
}

void setCurrent(Instance* instance) {
    const std::uintptr_t me = thisThread();
    if (Slot* slot = slotOf(me)) {
        slot->instance = instance;
        if (!instance) slot->thread.store(0, std::memory_order_release);
        return;
    }
    if (!instance) return;
    for (int i = 0; i < kSlots; ++i) {
        std::uintptr_t free = 0;
        if (gSlots[i].thread.compare_exchange_strong(free, me, std::memory_order_acq_rel)) {
            gSlots[i].instance = instance;
            int highest = gHighest.load(std::memory_order_relaxed);
            while (highest < i + 1 &&
                   !gHighest.compare_exchange_weak(highest, i + 1, std::memory_order_release));
            return;
        }
    }
    // More threads inside hooks at once than slots: the built-ins fall back
    // to the default instance's rate.
}

ParamRegistrar* registrar() { return tRegistrar; }
void setRegistrar(ParamRegistrar* registrar) { tRegistrar = registrar; }

void outOfRange(int line) {
    Instance* in = current();
    if (!in) return;
    if (in->status.outOfRange.fetch_add(1, std::memory_order_relaxed) == 0)
        in->status.outOfRangeLine.store(static_cast<std::uint32_t>(line),
                                        std::memory_order_relaxed);
}

static double rate() {
    const Instance* found = current();
    const Instance* in = found ? found : &gDefault;
    return in->sampleRate;
}

}  // namespace detail

// --- numbers -------------------------------------------------------------------

double sin(double x) { return std::sin(x); }
double cos(double x) { return std::cos(x); }
double tan(double x) { return std::tan(x); }
double asin(double x) { return std::asin(x); }
double acos(double x) { return std::acos(x); }
double atan(double x) { return std::atan(x); }
double atan2(double y, double x) { return std::atan2(y, x); }
double sinh(double x) { return std::sinh(x); }
double cosh(double x) { return std::cosh(x); }
double tanh(double x) { return std::tanh(x); }
double exp(double x) { return std::exp(x); }
double exp2(double x) { return std::exp2(x); }
double log(double x) { return std::log(x); }
double log2(double x) { return std::log2(x); }
double log10(double x) { return std::log10(x); }
double pow(double x, double y) { return std::pow(x, y); }
double sqrt(double x) { return std::sqrt(x); }
double cbrt(double x) { return std::cbrt(x); }
double floor(double x) { return std::floor(x); }
double ceil(double x) { return std::ceil(x); }
double trunc(double x) { return std::trunc(x); }
double round(double x) { return std::round(x); }
double fmod(double x, double y) { return std::fmod(x, y); }
double hypot(double x, double y) { return std::hypot(x, y); }

double dbToGain(double db) { return dsp::dbToGain(db); }
double gainToDb(double gain) { return dsp::gainToDb(gain); }
double mtof(double note) { return 440.0 * std::exp2((note - 69.0) / 12.0); }
double ftom(double hz) { return 69.0 + 12.0 * std::log2(hz / 440.0); }

namespace {
int saturate(double x) {
    if (!(x == x)) return 0;
    if (x >= 2147483647.0) return INT_MAX;
    if (x <= -2147483648.0) return INT_MIN;
    return static_cast<int>(x);
}
}  // namespace

int toInt(double x) { return saturate(std::trunc(x)); }
int floorToInt(double x) { return saturate(std::floor(x)); }
int ceilToInt(double x) { return saturate(std::ceil(x)); }
int roundToInt(double x) { return saturate(std::round(x)); }
int idiv(int a, int b) {
    if (b == 0) return 0;
    if (b == -1) return static_cast<int>(0u - static_cast<unsigned>(a));
    return a / b;
}
int imod(int a, int b) {
    if (b == 0 || b == -1) return 0;
    return a % b;
}
int wrap(int i, int n) {
    if (n <= 0) return 0;
    const int r = i % n;
    return r < 0 ? r + n : r;
}

// --- containers ----------------------------------------------------------------

template <class T>
Buffer<T>& Buffer<T>::operator=(const Buffer& other) {
    if (this == &other) return *this;
    if (size_ != other.size_) {
        std::free(items_);
        items_ =
            other.size_ > 0
                ? static_cast<T*>(std::calloc(static_cast<std::size_t>(other.size_), sizeof(T)))
                : nullptr;
        size_ = items_ ? other.size_ : 0;
    }
    if (size_ > 0) std::memcpy(items_, other.items_, static_cast<std::size_t>(size_) * sizeof(T));
    return *this;
}

template <class T>
Buffer<T>::~Buffer() {
    std::free(items_);
}

template <class T>
void Buffer<T>::resize(int size) {
    // A bound, not a policy: 64 M elements is far beyond any delay or table
    // a plugin needs, and keeps a mistaken size from exhausting memory.
    constexpr int kMax = 1 << 26;
    size = size < 0 ? 0 : (size > kMax ? kMax : size);
    if (size == size_) return;
    T* grown = size > 0 ? static_cast<T*>(std::calloc(static_cast<std::size_t>(size), sizeof(T)))
                        : nullptr;
    if (size > 0 && !grown) return;  // out of memory: keep what there was
    const int keep = size < size_ ? size : size_;
    if (keep > 0) std::memcpy(grown, items_, static_cast<std::size_t>(keep) * sizeof(T));
    std::free(items_);
    items_ = grown;
    size_ = size;
}

template class Buffer<double>;
template class Buffer<float>;
template class Buffer<int>;

// --- parameters ----------------------------------------------------------------

Param::Param(const ParamSpec& spec) {
    value_ = spec.def;
    min_ = spec.min;
    offAtMinimum_ = spec.offAtMinimum;
    if (auto* r = detail::registrar()) r->declare(*this, spec);
}

Param::Param(const Param& other) { *this = other; }

Param& Param::operator=(const Param& other) {
    value_ = other.value_;
    glide_ = other.glide_;
    glideFrames_ = other.glideFrames_;
    changed_ = other.changed_;
    offAtMinimum_ = other.offAtMinimum_;
    min_ = other.min_;
    reported_ = other.reported_;
    slot_ = other.slot_;
    return *this;
}

double Param::gain() const {
    if (offAtMinimum_ && value_ <= min_) return 0.0;
    return value_ == 0.0 ? 1.0 : std::pow(10.0, value_ / 20.0);
}

// --- the plugin ----------------------------------------------------------------

namespace {
const detail::Instance& instanceOf(detail::Instance* in) {
    static const detail::Instance none;
    return in ? *in : none;
}
}  // namespace

double Plugin::sampleRate() const { return instanceOf(instance_).sampleRate; }
int Plugin::maxBlockSize() const { return instanceOf(instance_).maxBlockSize; }
int Plugin::channels() const { return instanceOf(instance_).channels; }
bool Plugin::offline() const { return instanceOf(instance_).offline; }

// --- DSP -------------------------------------------------------------------------

namespace {

// Each object's state lives in the storage its class declares in script.h;
// these are what that storage holds.
struct BiquadState {
    dsp::Biquad coefficients;
    dsp::Biquad::State state;
};
struct OnePoleState {
    dsp::OnePole filter;
    dsp::OnePole::State state;
};
struct SmootherState {
    double current = 0.0, target = 0.0, coefficient = 0.0, ms = 0.0;
};

template <class T, class Storage>
T& as(Storage& storage) {
    static_assert(sizeof(T) <= sizeof(storage),
                  "a built-in object's storage in script.h is too small");
    static_assert(alignof(T) <= 8, "a built-in object needs more alignment than script.h gives it");
    return *std::launder(reinterpret_cast<T*>(&storage));
}
template <class T, class Storage>
const T& as(const Storage& storage) {
    return *std::launder(reinterpret_cast<const T*>(&storage));
}

}  // namespace

Biquad::Biquad() { new (storage_) BiquadState(); }
void Biquad::lowpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::lowpass(hz, q, detail::rate());
}
void Biquad::highpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::highpass(hz, q, detail::rate());
}
void Biquad::bandpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::bandpass(hz, q, detail::rate());
}
void Biquad::notch(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::notch(hz, q, detail::rate());
}
void Biquad::allpass(double hz, double q) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::allpass(hz, q, detail::rate());
}
void Biquad::peak(double hz, double q, double gainDb) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::peak(hz, q, gainDb, detail::rate());
}
void Biquad::lowShelf(double hz, double q, double gainDb) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::lowShelf(hz, q, gainDb, detail::rate());
}
void Biquad::highShelf(double hz, double q, double gainDb) {
    as<BiquadState>(storage_).coefficients = dsp::Biquad::highShelf(hz, q, gainDb, detail::rate());
}
double Biquad::process(double x) {
    auto& b = as<BiquadState>(storage_);
    return b.coefficients.tick(b.state, x);
}
void Biquad::clear() { as<BiquadState>(storage_).state = {}; }
double Biquad::responseDb(double hz) const {
    return 20.0 * std::log10(std::abs(
                      as<BiquadState>(storage_).coefficients.response(hz, detail::rate())));
}

OnePole::OnePole() { new (storage_) OnePoleState(); }
void OnePole::setCutoff(double hz) {
    as<OnePoleState>(storage_).filter.setCutoff(hz, detail::rate());
}
double OnePole::lowpass(double x) {
    auto& f = as<OnePoleState>(storage_);
    return f.filter.lowpass(f.state, x);
}
double OnePole::highpass(double x) {
    auto& f = as<OnePoleState>(storage_);
    return f.filter.highpass(f.state, x);
}
void OnePole::clear() { as<OnePoleState>(storage_).state = {}; }

// A resource: the line's buffer is allocated, so the object holds it by
// pointer. Constructed with the plugin (main thread); prepare() sizes it.
namespace {
dsp::FractionalDelay& line(void* p) { return *static_cast<dsp::FractionalDelay*>(p); }
}  // namespace
DelayLine::DelayLine() : line_(new dsp::FractionalDelay()) {}
DelayLine::DelayLine(const DelayLine& other) : line_(new dsp::FractionalDelay(line(other.line_))) {}
DelayLine& DelayLine::operator=(const DelayLine& other) {
    // Restoring a plugin's state copies lines of equal capacity, which
    // std::vector does in place; only a differently sized line allocates.
    if (this != &other) line(line_) = line(other.line_);
    return *this;
}
DelayLine::~DelayLine() { delete &line(line_); }
void DelayLine::prepare(int maxDelaySamples) {
    line(line_).prepare(maxDelaySamples < 0 ? 0 : maxDelaySamples);
}
void DelayLine::push(double x) { line(line_).push(x); }
double DelayLine::read(double delaySamples) const {
    // The framework's line converts the delay to an index: NaN must not reach it.
    if (!(delaySamples == delaySamples)) delaySamples = 0.0;
    return line(line_).read(delaySamples);
}
void DelayLine::clear() { line(line_).clear(); }

Random::Random() { new (storage_) dsp::Random(); }
void Random::seed(int seed) {
    as<dsp::Random>(storage_).reseed(static_cast<std::uint64_t>(static_cast<unsigned>(seed)));
}
double Random::next() { return as<dsp::Random>(storage_).uniform(); }
double Random::bipolar() { return as<dsp::Random>(storage_).bipolar(); }
double Random::gaussian() { return as<dsp::Random>(storage_).gaussian(); }
int Random::below(int n) {
    return n <= 0
               ? 0
               : static_cast<int>(as<dsp::Random>(storage_).below(static_cast<std::uint32_t>(n)));
}

Noise::Noise() { new (storage_) dsp::Noise(); }
void Noise::seed(int seed) {
    as<dsp::Noise>(storage_).reseed(static_cast<std::uint64_t>(static_cast<unsigned>(seed)));
}
double Noise::white() { return as<dsp::Noise>(storage_).white(); }
double Noise::pink() { return as<dsp::Noise>(storage_).pink(); }
double Noise::brown() { return as<dsp::Noise>(storage_).brown(); }

Oscillator::Oscillator() { new (storage_) dsp::Oscillator(); }
void Oscillator::setShape(Shape shape) {
    as<dsp::Oscillator>(storage_).setShape(
        static_cast<dsp::Oscillator::Shape>(static_cast<int>(shape)));
}
void Oscillator::setFrequency(double hz) {
    as<dsp::Oscillator>(storage_).setFrequency(hz, detail::rate());
}
void Oscillator::setPulseWidth(double width) { as<dsp::Oscillator>(storage_).setPulseWidth(width); }
void Oscillator::setPhase(double phase) { as<dsp::Oscillator>(storage_).setPhase(phase); }
double Oscillator::phase() const { return as<dsp::Oscillator>(storage_).phase(); }
double Oscillator::next() { return as<dsp::Oscillator>(storage_).tick(); }

// Constructors depend on nothing but the type: a new object is the same
// whatever is running (the code generator may copy a constructed one).
Adsr::Adsr() { new (storage_) dsp::Adsr(); }
void Adsr::set(double attackMs, double decayMs, double sustain, double releaseMs) {
    as<dsp::Adsr>(storage_).setTimes(attackMs, decayMs, sustain, releaseMs, detail::rate());
}
void Adsr::setCurves(double attack, double decayRelease) {
    as<dsp::Adsr>(storage_).setCurves(attack, decayRelease);
}
void Adsr::gate(bool on) { as<dsp::Adsr>(storage_).gate(on); }
void Adsr::retrigger() { as<dsp::Adsr>(storage_).retrigger(); }
void Adsr::kill() { as<dsp::Adsr>(storage_).kill(); }
double Adsr::next() { return as<dsp::Adsr>(storage_).tick(); }
double Adsr::level() const { return as<dsp::Adsr>(storage_).level(); }
bool Adsr::active() const { return as<dsp::Adsr>(storage_).active(); }

}  // namespace supple::script
