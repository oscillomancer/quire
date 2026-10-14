// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "bridge_unavailable.h"
#include "platform.h"
namespace supple::host {
namespace {
std::filesystem::path executable;
Error unavailable() {
    return Error("host.bridge.unsupported", "plugin isolation is unavailable on this platform",
                 "/isolation", "select in_process isolation until the native bridge is available");
}
}  // namespace
Result<std::unique_ptr<RemoteInstance>> RemoteInstance::spawn(const PluginInfo&,
                                                              const HostIdentity&, RemotePorts&,
                                                              std::shared_ptr<BridgeProcess>) {
    return unavailable();
}
Result<std::shared_ptr<BridgeProcess>> createBridgeProcess() { return unavailable(); }
bool bridgeProcessFailed(const std::shared_ptr<BridgeProcess>&) { return false; }
void setBridgeExecutable(std::filesystem::path path) { executable = std::move(path); }
std::filesystem::path bridgeExecutable() { return executable; }
bool locateBridgeExecutable(std::string_view name) {
    const auto dir = platform::executablePath().parent_path();
    auto file = platform::pathFromText(name);
#if defined(_WIN32)
    file += ".exe";
#endif
    for (const auto& candidate : {dir / file, dir.parent_path() / "clap-host" / file,
                                  dir.parent_path().parent_path() / "host/clap-host" / file}) {
        if (platform::executable(candidate)) {
            setBridgeExecutable(candidate);
            return true;
        }
    }
    return false;
}
}  // namespace supple::host
