#include "supple/agent/config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <system_error>

#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "supple/json/write.h"

namespace supple::agent {
namespace {

Error writeError(const std::filesystem::path& file, int error) {
    return Error("agent.config",
                 "cannot write " + file.string() + ": " + std::generic_category().message(error),
                 {}, "check that the configuration directory exists and is writable");
}

// Publishes `text` at `file` without ever exposing a partial or world-readable
// file: a temporary file in the same directory, created exclusively and 0600
// before any secret is written into it, written in full and fsynced, renamed
// over the target, then the directory fsynced. Any failure removes the
// temporary file and leaves the previous configuration untouched.
Status writePrivate(const std::filesystem::path& file, std::string_view text) {
    if (!file.has_filename()) return writeError(file, EINVAL);
    const std::filesystem::path dir = file.has_parent_path() ? file.parent_path() : ".";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string temp = (dir / ("." + file.filename().string() + ".XXXXXX")).string();
    const int fd = ::mkostemp(temp.data(), O_CLOEXEC);  // O_EXCL, mode 0600
    if (fd < 0) return writeError(file, errno);
    auto fail = [&](int error) {
        ::close(fd);
        ::unlink(temp.c_str());
        return writeError(file, error);
    };
    if (::fchmod(fd, 0600) != 0) return fail(errno);  // whatever the C library chose
    for (std::size_t done = 0; done < text.size();) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(n < 0 ? errno : EIO);
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) return fail(errno);
    if (::close(fd) != 0) {
        const int error = errno;
        ::unlink(temp.c_str());
        return writeError(file, error);
    }
    if (::rename(temp.c_str(), file.c_str()) != 0) {
        const int error = errno;
        ::unlink(temp.c_str());
        return writeError(file, error);
    }
    // The rename is durable once the directory is; a failure here leaves the
    // new file in place (readers already see it), so it is not reported.
    if (const int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); d >= 0) {
        ::fsync(d);
        ::close(d);
    }
    return {};
}

}  // namespace

std::filesystem::path Config::defaultFile(std::string_view slug) {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / slug / "agent.json";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".config" / slug / "agent.json";
    return {};
}

Result<Config> Config::load(const std::filesystem::path& file) {
    Config c;
    if (file.empty() || !std::filesystem::exists(file)) return c;
    auto bytes = readFile(file);
    if (!bytes) return std::move(bytes).error();
    auto doc =
        json::parse(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()));
    if (!doc) return std::move(doc).error();
    if (at(*doc, "schema").isString() == false ||
        at(*doc, "schema").asString() != "supple.agent/1.0")
        return Error("agent.config", file.string() + " is not a supple.agent/1.0 document",
                     "/schema");
    const json::Value& list = at(*doc, "endpoints");
    if (list.isArray())
        for (std::size_t i = 0; i < list.asArray().size(); ++i) {
            const json::Value& e = list.asArray()[i];
            const std::string path = "/endpoints/" + std::to_string(i);
            Endpoint ep;
            const std::string kind =
                at(e, "kind").isString() ? at(e, "kind").asString() : "anthropic";
            if (kind != "anthropic" && kind != "openai")
                return Error("agent.config", "kind is \"anthropic\" or \"openai\"", path + "/kind");
            ep.kind = kind == "openai" ? Endpoint::Kind::OpenAI : Endpoint::Kind::Anthropic;
            for (auto [key, field] : {std::pair{"name", &ep.name},
                                      {"base_url", &ep.baseUrl},
                                      {"model", &ep.model},
                                      {"api_key", &ep.apiKey}})
                if (at(e, key).isString()) *field = at(e, key).asString();
            if (at(e, "max_tokens").isNumber())
                ep.maxTokens = static_cast<int>(at(e, "max_tokens").asInt());
            c.endpoints.push_back(std::move(ep));
        }
    if (at(*doc, "current").isNumber())
        c.current = static_cast<std::size_t>(at(*doc, "current").asInt());
    return c;
}

Status Config::save(const std::filesystem::path& file) const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.agent/1.0";
    json::Value list = json::Value::array();
    for (const Endpoint& e : endpoints) {
        json::Value o = json::Value::object();
        o["name"] = e.name;
        o["kind"] = e.kind == Endpoint::Kind::OpenAI ? "openai" : "anthropic";
        o["base_url"] = e.baseUrl;
        o["model"] = e.model;
        o["api_key"] = e.apiKey;
        o["max_tokens"] = e.maxTokens;
        list.push_back(std::move(o));
    }
    doc["endpoints"] = std::move(list);
    doc["current"] = static_cast<std::int64_t>(current);
    return writePrivate(file, json::writeCanonical(doc) + "\n");
}

}  // namespace supple::agent
