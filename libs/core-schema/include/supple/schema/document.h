#pragma once
#include <supple/schema/schema.h>
#include <exception>
#include <functional>
namespace supple::schema {
struct DocumentId {
    std::string ns = "supple";
    std::string type;
    int major = 0;
    int minor = 0;
};
Result<DocumentId> parseDocumentId(std::string_view text);
std::string formatDocumentId(const DocumentId& id);
using Migration = std::function<Result<json::Value>(json::Value)>;

// migrations[k - 1] upgrades a whole document from major k to k + 1.
template <class T>
class DocumentType {
  public:
    DocumentType(std::string type, int major, int minor, Schema<T> body,
                 std::vector<Migration> migrations = {})
        : id_{"supple", std::move(type), major, minor},
          body_(std::move(body)),
          migrations_(std::move(migrations)) {}
    Result<T> fromJson(const json::Value& input, std::vector<Warning>* warnings = nullptr) const {
        if (!input.isObject())
            return Error("document.missing_id", "document must be an object with a schema member",
                         "/schema", "add \"schema\": \"" + formatDocumentId(id_) + "\"");
        const auto* schema = input.find("schema");
        if (!schema)
            return Error("document.missing_id", "document is missing its schema member", "/schema",
                         "add \"schema\": \"" + formatDocumentId(id_) + "\"");
        if (!schema->isString())
            return Error("document.bad_id", "schema member must be a document ID string", "/schema",
                         "use \"" + formatDocumentId(id_) + "\"");
        auto parsed = parseDocumentId(schema->asString());
        if (!parsed) {
            auto error = parsed.error();
            return std::move(error).at("/schema");
        }
        if (parsed->ns != id_.ns || parsed->type != id_.type)
            return Error("document.wrong_type", "document has the wrong schema type", "/schema",
                         "expected \"" + formatDocumentId(id_) + "\"");
        if (parsed->major > id_.major)
            return Error("document.too_new", "document major version is newer than supported",
                         "/schema",
                         "use a reader for major version " + std::to_string(parsed->major));
        json::Value migrated = input;
        int major = parsed->major;
        if (major < id_.major) {
            for (; major < id_.major; ++major) {
                const std::size_t index = static_cast<std::size_t>(major - 1);
                if (major < 1 || index >= migrations_.size() || !migrations_[index])
                    return Error("document.migration_failed",
                                 "no migration for document major version " + std::to_string(major),
                                 "/schema",
                                 "provide a migration from major " + std::to_string(major) +
                                     " to " + std::to_string(major + 1));
                auto next = [&]() -> Result<json::Value> {
                    try {
                        return migrations_[index](std::move(migrated));
                    } catch (const std::exception& exception) {
                        return Error("document.migration_failed", "migration threw an exception",
                                     "/schema", exception.what());
                    } catch (...) {
                        return Error("document.migration_failed", "migration threw an exception",
                                     "/schema", "fix the migration function");
                    }
                }();
                if (!next) {
                    Error error("document.migration_failed", "document migration failed", "/schema",
                                "fix the migration from major " + std::to_string(major));
                    error.details.push_back(next.error());
                    return error;
                }
                migrated = std::move(*next);
                const auto* nextId = migrated.find("schema");
                auto checked =
                    nextId && nextId->isString()
                        ? parseDocumentId(nextId->asString())
                        : Result<DocumentId>(Error("document.bad_id", "migration omitted schema"));
                if (!checked || checked->ns != id_.ns || checked->type != id_.type ||
                    checked->major != major + 1)
                    return Error("document.migration_failed",
                                 "migration returned the wrong document ID", "/schema",
                                 "write the next major document ID");
            }
        }
        auto version = parseDocumentId(migrated.find("schema")->asString());
        json::Value body = migrated;
        body.asObject().erase("schema");
        DecodeOptions options;
        if (version && version->major == id_.major && version->minor > id_.minor)
            options.unknownMembers = UnknownMembers::Ignore;
        return body_.decode(body, options, warnings);
    }
    Result<T> parse(std::string_view text, std::vector<Warning>* warnings = nullptr) const {
        auto value = json::parse(text);
        if (!value) return value.error();
        return fromJson(*value, warnings);
    }
    json::Value toJson(const T& value) const {
        json::Value result = json::Value::object();
        result["schema"] = formatDocumentId(id_);
        auto body = body_.encode(value);
        if (body.isObject())
            for (const auto& member : body.asObject())
                if (member.key != "schema") result[member.key] = member.value;
        return result;
    }
    std::string serialize(const T& value) const { return json::writeCanonical(toJson(value)); }
    json::Value jsonSchema() const {
        json::Value result = body_.jsonSchema();
        json::Value properties = json::Value::object();
        properties["schema"]["const"] = formatDocumentId(id_);
        if (const auto* bodyProperties = result.find("properties");
            bodyProperties && bodyProperties->isObject())
            for (const auto& member : bodyProperties->asObject())
                if (member.key != "schema") properties[member.key] = member.value;
        result["properties"] = std::move(properties);
        json::Value required = json::Value::array();
        required.push_back("schema");
        if (const auto* bodyRequired = result.find("required");
            bodyRequired && bodyRequired->isArray())
            for (const auto& member : bodyRequired->asArray()) required.push_back(member);
        result["required"] = std::move(required);
        return result;
    }
    const DocumentId& id() const { return id_; }

  private:
    DocumentId id_;
    Schema<T> body_;
    std::vector<Migration> migrations_;
};
}  // namespace supple::schema
