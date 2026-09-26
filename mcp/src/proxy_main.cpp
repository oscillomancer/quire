#include <supple/mcp/server.h>
#include <supple/product.h>
#include <cstdio>
#include <iostream>

int main(int argc, char** argv) {
    using namespace supple;
    std::filesystem::path path;
    if (argc == 3 && std::string_view(argv[1]) == "--socket")
        path = argv[2];
    else if (argc == 1)
        path = rpc::socketPath("main");
    else {
        std::fprintf(stderr, "usage: %.*s-mcp [--socket PATH]\n",
                     static_cast<int>(product::kSlug.size()), product::kSlug.data());
        return 2;
    }
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
