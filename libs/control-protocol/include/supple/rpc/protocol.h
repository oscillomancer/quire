#pragma once

#include <supple/cmd/bus.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace supple::rpc {
inline constexpr std::size_t kMaxMessageBytes = 16 * 1024 * 1024;
using Post = std::function<void(std::function<void()>)>;

json::Value errorJson(const Error& error);
Error errorFromJson(const json::Value& value);
std::filesystem::path socketPath(std::string_view instance);

// The I/O thread owns sockets and framing. `post` schedules every request on
// the caller's model thread; the caller must drain that queue until stop().
class Server {
  public:
    Server(const cmd::Registry& registry, cmd::Dispatcher& dispatcher, Post post);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Status start(const std::filesystem::path& path);
    void stop();
    // Called on the model thread by the host's event sink.
    void publish(std::string_view topic, const json::Value& payload);
    std::filesystem::path path() const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

// Synchronous client for the MCP proxy and protocol tests. Calls must be made
// off the model thread when connected to a server using that same thread.
class Client {
  public:
    Client() = default;
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Status connect(const std::filesystem::path& path, std::string client, std::string agent = {},
                   bool mcp = false);
    void close();
    Result<json::Value> call(std::string_view method,
                             const json::Value& params = json::Value::object());

  private:
    int fd_ = -1;
    std::uint64_t nextId_ = 1;
    std::string input_;
};
}  // namespace supple::rpc
