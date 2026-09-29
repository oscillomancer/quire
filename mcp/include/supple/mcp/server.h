#pragma once

#include <supple/rpc/protocol.h>
#include <chrono>
#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <ostream>

namespace supple::mcp {
// A service tick run on the serving thread whenever no request is waiting (the
// stdio reader polls, so it never blocks the tick).
struct Tick {
    std::function<void()> fn;
    std::chrono::milliseconds interval{100};
};

// MCP over newline-delimited JSON-RPC on stdio. The in-process deployment
// executes on the caller's model thread; the proxy forwards through rpc::Client.
class Server {
  public:
    Server(const cmd::Registry& registry, cmd::Dispatcher& dispatcher);
    static Result<std::unique_ptr<Server>> proxy(rpc::Client& client);
    Status serve(std::istream& input, std::ostream& output, Tick tick = {});
    // As above, reading requests from `fd` so it can be polled with the tick's
    // deadline (a negative fd reads blocking).
    Status serve(int fd, std::ostream& output, Tick tick = {});
    std::optional<json::Value> handle(const json::Value& request);

  private:
    explicit Server(rpc::Client& client);
    Result<json::Value> call(std::string_view name, const json::Value& args);
    json::Value tools() const;
    std::function<Result<json::Value>(std::string_view, const json::Value&, const cmd::Origin&)>
        invoke_;
    json::Value dedicated_ = json::Value::array();
    std::map<std::string, std::string> commands_;
    std::map<std::string, cmd::Kind> kinds_;
    rpc::Client* proxyClient_ = nullptr;
    bool queryTool_ = false;
    bool initialized_ = false;
    cmd::Origin origin_{cmd::Origin::Type::Mcp, {}, {}};
};
}  // namespace supple::mcp
