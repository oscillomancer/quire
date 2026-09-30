# Delay integration

The template was instantiated on `p3-delay` with functional identity `delay`,
CLAP `com.suppleaudio.delay`, state `QDL1`, AU `QDly`, audio-effect/delay/stereo
features. Display name is defined once in `cmake/Identity.cmake`. MIT stays
under ADR-0011; upstream licensing instructions do not override this repo.

An SDK-free engine replaces the placeholder gain processor. It owns float
circular lines, cubic reads, sample-counted two-pole time glide, local TPT
loop filters, deterministic wow, safety saturation, ducking and frozen ring
views. There is no sibling-engine link or shared DSP edit. `prepare` allocates;
RT processing/reset/parameter updates have bounded storage and no allocations.
Implementation choices and limits are recorded in stock-effects §6.

Product wrapper sections follow the reviewed compressor integration:
ParamStore, ClapParams, FlatSemantic and framed transactional JSON state,
normalized host floats, sample-offset events, mono/stereo float/double and
in-place processing. Describe is limited to 300 characters. The main bus is
one input/output; no key port. Tail changes use atomic publication and notify
on the audio thread from process, including transport tempo, automation,
semantic patches and state restoration. Latency is zero and no restart is
requested. The tail/transport host test covers the approved thread correction.

The current template window, GUI bridge, platform renderers, entry points,
host_contract, Linux embedding probe, fonts and Ring are retained after names
(including vendor namespace qualification). Frame payload adds timing, tempo
and duck telemetry. Product-owned Editor/Theme/Widgets use the stock palette,
values below knobs and centre-origin bipolar arcs. Native previews embed in a
fixed-size Linux parent to avoid tiling-WM distortion. The generic CLAP probe's
meter report is omitted because this effect has GUI telemetry only.

CMake follows the stock ExternalProject build with local vendored paths,
C++20, engine tests under all sanitizer presets and semantic/editor/wrapper/
host tests. The current template's export maps are retained and its CLAP
export check runs even without VST3. No template/dependency source is edited;
VST3's self-handle/unload validation remains optional platform acceptance.
The obsolete placeholder CLI/DSP and example GUI image are removed.

Useful template seams: engine-owned describe, physical parameter integration,
transport-aware timing and the CLAP tail contract. STATUS, independent review
and integration belong to the orchestrator; REAPER/platform acceptance needs
the separate requested host/device setup.
