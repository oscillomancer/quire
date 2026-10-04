// An agent session (docs/specs/gui.md §5): the model works through the same
// MCP tool surface as external agents, on its own branch; the human reviews
// the branch's diff, A/Bs it against the base and accepts or rejects it.
// The model's requests run on a worker thread; every command runs on the
// model thread (through `post`), recorded with Origin::Type::Agent.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
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
    std::function<void()> idle;             // the request is done (or failed, or cancelled)
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
    void work(std::stop_token stop, std::weak_ptr<int> alive);
    Result<json::Value> exec(const std::string& command, json::Value args);  // model thread
    ToolResult call(const ToolCall& call);                                   // model thread
    Status ensureBranch();                                                   // model thread

    Endpoint endpoint_;
    cmd::Dispatcher& dispatcher_;
    Post post_;
    Options options_;
    std::unique_ptr<mcp::Server> mcp_;
    std::vector<ToolDef> tools_;
    std::string system_;
    std::string branch_, base_;
    mutable std::mutex mutex_;
    std::vector<Message> history_;
    std::atomic<bool> busy_{false};
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
    std::jthread worker_;
    int nextId_ = 1;
};

}  // namespace supple::agent
