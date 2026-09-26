#include <supple/schema/document.h>
#include <charconv>
namespace supple::schema {
namespace {
bool validName(std::string_view name) {
    if (name.empty() || name.front() < 'a' || name.front() > 'z') return false;
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}
}  // namespace
Result<DocumentId> parseDocumentId(std::string_view text) {
    auto dot = text.find('.');
    auto slash = text.find('/');
    if (dot == std::string_view::npos || slash == std::string_view::npos || dot >= slash)
        return Error("document.bad_id", "invalid document schema ID", {},
                     "use a name and version such as supple.eq-patch/1.0");
    DocumentId id;
    id.ns = text.substr(0, dot);
    id.type = text.substr(dot + 1, slash - dot - 1);
    auto version = text.substr(slash + 1);
    auto separator = version.find('.');
    if (!validName(id.ns) || !validName(id.type) || separator == std::string_view::npos)
        return Error("document.bad_id", "invalid document schema ID", {},
                     "use a name and version such as supple.eq-patch/1.0");
    auto first = version.substr(0, separator);
    auto second = version.substr(separator + 1);
    auto [majorEnd, majorError] =
        std::from_chars(first.data(), first.data() + first.size(), id.major);
    auto [minorEnd, minorError] =
        std::from_chars(second.data(), second.data() + second.size(), id.minor);
    if (first.empty() || second.empty() || majorError != std::errc{} || minorError != std::errc{} ||
        majorEnd != first.data() + first.size() || minorEnd != second.data() + second.size() ||
        id.major < 1 || id.minor < 0)
        return Error("document.bad_id", "invalid document version", {},
                     "use positive major and nonnegative minor numbers, e.g. /1.0");
    return id;
}
std::string formatDocumentId(const DocumentId& id) {
    return id.ns + "." + id.type + "/" + std::to_string(id.major) + "." + std::to_string(id.minor);
}
}  // namespace supple::schema
