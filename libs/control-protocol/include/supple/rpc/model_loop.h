#pragma once

#include <functional>

namespace supple::rpc {
// Executes posted socket requests on the headless host's model thread.
class ModelLoop {
  public:
    ModelLoop();
    ~ModelLoop();
    ModelLoop(const ModelLoop&) = delete;
    ModelLoop& operator=(const ModelLoop&) = delete;
    void post(std::function<void()> task);
    void run();  // blocks until SIGINT or SIGTERM

  private:
    struct Impl;
    Impl* impl_;
};
}  // namespace supple::rpc
