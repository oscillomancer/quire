#include <supple/cmd/bus.h>
#include <cstddef>
#include <cstdint>

namespace {
struct Args {
    int amount = 0;
};
struct Result {
    int total = 0;
};
struct Host final : supple::cmd::Host {
    int total = 0, prior = 0;
    supple::cmd::Context makeContext(const supple::cmd::Origin& origin) override {
        supple::cmd::Context ctx(origin);
        ctx.provide(total);
        return ctx;
    }
    supple::Result<std::uint64_t> beginTransaction(const supple::cmd::Origin&,
                                                   std::string_view) override {
        prior = total;
        return std::uint64_t{1};
    }
    supple::Status commitTransaction(std::uint64_t) override { return {}; }
    void rollbackTransaction(std::uint64_t) override { total = prior; }
    void log(const supple::cmd::LogRecord&) override {}
    void emit(std::string_view, supple::json::Value) override {}
};
}  // namespace
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 16384) return 0;
    auto parsed = supple::json::parse(std::string_view(reinterpret_cast<const char*>(data), size));
    if (!parsed || !parsed->isObject()) return 0;
    supple::cmd::Registry registry;
    supple::cmd::Command<Args, Result> command;
    command.name = "toy.add";
    command.kind = supple::cmd::Kind::Edit;
    command.exposure = supple::cmd::Exposure::Internal;
    command.summary = "Add an amount.";
    command.args = supple::schema::object<Args>("args")
                       .field("amount", &Args::amount, supple::schema::integerOf<int>())
                       .build();
    command.result = supple::schema::object<Result>("result")
                         .field("total", &Result::total, supple::schema::integerOf<int>())
                         .build();
    command.run = [](const Args& args, supple::cmd::Context& ctx) -> supple::Result<Result> {
        return Result{ctx.service<int>() += args.amount};
    };
    registry.add(std::move(command));
    Host host;
    supple::cmd::Dispatcher dispatcher(registry, host);
    auto* name = parsed->find("command");
    std::string commandName = name && name->isString() ? name->asString() : "batch";
    auto* args = parsed->find("args");
    auto output = dispatcher.execute(
        {commandName, args ? *args : *parsed, {supple::cmd::Origin::Type::Test, "fuzz", ""}});
    (void)output;
    return 0;
}
