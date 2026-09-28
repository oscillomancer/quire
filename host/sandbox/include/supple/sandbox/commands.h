// The sandbox's commands on the command bus (spec: docs/specs/sandbox.md §4)
// and the bus Host that runs them against a Session.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/lua/runtime.h"
#include "supple/sandbox/session.h"

namespace supple::sandbox {

// Plugins the example fixtures load (docs examples and example tests):
// "synth_loaded" = the instrument as in_1 and the effect as in_2;
// "synth_and_render" = the same plus one render rd_1 of a short phrase.
struct ExamplePlugins {
    std::string instrument = "com.suppleaudio.test.sine";
    std::string effect = "com.suppleaudio.test.gain";
};

// Registers every sandbox command (plugin.*, param.*, patch.*, preset.*,
// render, session.*) into `registry`.
void registerCommands(cmd::Registry& registry, const ExamplePlugins& examples = {});

// The bus Host: owns the Session, maps bus transactions onto Session
// transactions, appends every LogRecord to an NDJSON command log, and keeps
// emitted events for inspection. `render` is transaction-safe (a rolled-back
// batch deletes its renders).
class SandboxHost : public cmd::Host {
  public:
    // `commandLog`: NDJSON file to append to ("" = no file).
    SandboxHost(std::unique_ptr<Session> session, std::filesystem::path commandLog = {},
                ExamplePlugins examples = {});

    Session& session() { return *session_; }
    // The Lua runtime scripts run in (ADR-0006): attach the dispatcher once it
    // exists, then script.run works and scripts receive bus events.
    void attachCommands(cmd::Dispatcher& dispatcher);
    lua::Runtime* scripting() { return scripts_.get(); }
    const std::vector<cmd::LogRecord>& records() const { return records_; }
    std::vector<std::pair<std::string, json::Value>> takeEvents();
    // Tests: a fixed clock for deterministic logs.
    void setFixedClock(std::uint64_t ms) { fixedClock_ = ms; }

    cmd::Context makeContext(const cmd::Origin&) override;  // adopts background renders
    void stopJobs() override;
    Result<std::uint64_t> beginTransaction(const cmd::Origin&, std::string_view label) override;
    Status commitTransaction(std::uint64_t) override;
    void rollbackTransaction(std::uint64_t) override;
    void log(const cmd::LogRecord&) override;
    void emit(std::string_view topic, json::Value payload) override;
    void commandFinished() override;
    bool actionTransactional(std::string_view command) const override;
    std::uint64_t clockMs() const override;
    void setupFixture(cmd::Dispatcher&, std::string_view fixture) override;

  private:
    std::unique_ptr<Session> session_;
    std::filesystem::path commandLog_;
    ExamplePlugins examples_;
    std::vector<cmd::LogRecord> records_;
    std::vector<std::pair<std::string, json::Value>> events_;
    std::unique_ptr<lua::Runtime> scripts_;
    std::uint64_t nextTxn_ = 1, openTxn_ = 0;
    std::uint64_t fixedClock_ = 0;
};

json::Value toJson(const cmd::LogRecord& record);

}  // namespace supple::sandbox
