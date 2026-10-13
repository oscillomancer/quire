// The sandbox's DSP script commands (ADR-0032): dsp.write, dsp.check, dsp.get,
// dsp.list, dsp.test and dsp.export over the session (host/dsp-commands).
#include "dsp_commands.h"

#include "../../dsp-commands/dsp_export.h"
#include "../../dsp-commands/dsp_test.h"
#include "supple/sandbox/session.h"

namespace supple::dsp_commands {
template <>
struct TestTraits<sandbox::Session> {
    static sandbox::RenderSettings settings(const sandbox::Session& s) {
        sandbox::RenderSettings r;
        r.sampleRate = s.settings().sampleRate;
        r.blockSize = s.settings().blockSize;
        r.tailLimitSeconds = s.settings().tailLimitSeconds;
        return r;
    }
    static host::HostIdentity identity(const sandbox::Session& s) { return s.options().identity; }
    static std::filesystem::path outputDir(const sandbox::Session& s) {
        return s.options().workdir / "renders";
    }
};
}  // namespace supple::dsp_commands

namespace supple::sandbox {

void registerDspCommands(cmd::Registry& registry, const ExamplePlugins& ex) {
    dsp_commands::registerDspCommands<Session>(registry);
    dsp_commands::registerDspTest<Session>(registry, ex.effect);
    dsp_commands::registerDspExport<Session>(registry);
}

}  // namespace supple::sandbox
