#include <supple/mcp/server.h>
#include <supple/product.h>
#include <cstdio>
#include <iostream>

int main(int argc, char** argv) {
    using namespace supple;
    std::filesystem::path path;
    std::string instance = "default";
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--socket" && i + 1 < argc)
            path = argv[++i];
        else if (std::string_view(argv[i]) == "--instance" && i + 1 < argc)
            instance = argv[++i];
        else {
            std::fprintf(stderr, "usage: %.*s-mcp [--instance NAME] [--socket PATH]\n",
                         static_cast<int>(product::kSlug.size()), product::kSlug.data());
            return 2;
        }
    }
    if (path.empty()) path = rpc::socketPath(instance);
    rpc::Client client;
    auto connected = client.connect(path, "mcp-proxy", {}, true);
    if (!connected) {
        std::fprintf(stderr, "%s\n", connected.error().toString().c_str());
        return 1;
    }
    auto server = mcp::Server::proxy(client);
    if (!server) {
        std::fprintf(stderr, "%s\n", server.error().toString().c_str());
        return 1;
    }
    auto status = (*server)->serve(std::cin, std::cout);
    if (!status) {
        std::fprintf(stderr, "%s\n", status.error().toString().c_str());
        return 1;
    }
    return 0;
}
