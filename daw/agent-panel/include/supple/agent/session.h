// An agent session (docs/specs/gui.md §5): the model works through the same
// MCP tool surface as external agents, on its own branch; the human reviews
// the branch's diff, A/Bs it against the base and accepts or rejects it.
// The model's requests run on a worker thread; every command runs on the
// model thread (through `post`), recorded with Origin::Type::Agent.
//
// Isolation: the agent's tools refuse the commands that change which project
// or branch edits land on, or the app's lifecycle ("agent.denied": branch
// create/switch/merge/delete, project new/open, project.save with a path,
// engine start/stop), whether called directly, as a dedicated tool or inside
// a batch. Each turn pins the agent's branch and the project file; if either
// is no longer current when a tool call runs, or after it ran (a script, the
// human), the call fails with "agent.branch_changed" and the turn stops.
//
// Cancellation: each send starts a turn with a new generation; cancel()
// invalidates it, so tool calls and events the worker already posted become
// no-ops (a cancelled call reports "cancelled" to the model). busy() stays
// true, and accept/reject/audition stay refused, until the worker has stopped
// and its final `idle` event has run on the model thread.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "supple/agent/llm.h"
#include "supple/cmd/bus.h"

namespace supple::mcp {
class Server;
}

namespace supple::agent {

// What the panel shows; called on the model thread.
struct Events {
    std::function<void(std::string_view delta)> text;  // the assistant's words as they stream
    std::function<void(const ToolCall&)> toolStarted;
    std::function<void(const ToolCall&, const ToolResult&)> toolFinished;
    std::function<void(const json::Value& card)> listen;  // "ask the human to listen"
    std::function<void(const std::string& message)> error;
    std::function<void(const Turn&)> turn;  // one assistant turn finished
    std::function<void()> idle;  // the request is done (or failed, or cancelled); busy() is false
};

struct Options {
    std::filesystem::path skillDir;  // SKILL.md and guides/ (progressive disclosure)
    int maxToolRounds = 24;
    std::string client = "panel";  // Origin client name
};

class Session {
  public:
    using Post = std::function<void(std::function<void()>)>;
    Session(Endpoint endpoint, const cmd::Registry& registry, cmd::Dispatcher& dispatcher,
            Post post, Options options = {});
    ~Session();  // cancels and joins
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Events events;

    // Model thread. Starts working on `prompt`; false when already busy.
    bool send(std::string prompt);
    // Model thread. Stops the turn; `idle` follows once the worker is done.
    void cancel();
    bool busy() const { return busy_; }
    std::vector<Message> history() const;

    // Branch workflow (model thread). The branch is created on the first send.
    const std::string& branch() const { return branch_; }
    const std::string& base() const { return base_; }
    Result<json::Value> diff();         // branch.diff base → branch
    Status audition(bool agentBranch);  // A/B: switch playback and edits
    Result<json::Value> accept();       // merge into the base, back to the base, delete
    Status reject();                    // back to the base, delete the branch

    // The tools the model sees (MCP surface + skill_read + listen).
    const std::vector<ToolDef>& tools() const { return tools_; }
    const std::string& system() const { return system_; }

  private:
    void work(std::stop_token stop, std::weak_ptr<int> alive, std::uint64_t generation);
    // Model thread. `stopTurn` is set when the turn must end (agent.branch_changed).
    ToolResult call(const ToolCall& call, std::optional<Error>& stopTurn);
    Result<json::Value> exec(const std::string& command, json::Value args);  // model thread
    Status ensureBranch();                                                   // model thread
    Status admit(const ToolCall& call) const;                                // the isolation policy
    Status pinned();  // model thread: still on the agent's branch?

    Endpoint endpoint_;
    cmd::Dispatcher& dispatcher_;
    Post post_;
    Options options_;
    std::unique_ptr<mcp::Server> mcp_;
    std::vector<ToolDef> tools_;
    std::string system_;
    std::string branch_, base_;
    std::int64_t projectGeneration_ = 0;
    std::string projectPath_;                          // pinned at send
    std::map<std::string, std::string> toolCommands_;  // dedicated MCP tool → command
    std::uint64_t generation_ = 0;                     // model thread: the live turn
    mutable std::mutex mutex_;
    std::vector<Message> history_;
    std::atomic<bool> busy_{false};
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::jthread worker_;
    int nextId_ = 1;
};

}  // namespace supple::agent
