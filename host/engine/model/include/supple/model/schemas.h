// JSON forms of the session objects (core-schema) and the project file
// `supple.project/1.0` (session-model §5). Fields equal to their defaults are
// omitted, so files and diffs stay small; decoding fills them back in.
#pragma once

#include <string>
#include <string_view>

#include "supple/model/session.h"
#include "supple/schema/schema.h"

namespace supple::model {

schema::Schema<Id> idSchema();  // "tr_3"
template <class T>
const schema::Schema<T>& schemaOf();  // specialized for every object type

template <class T>
json::Value toJson(const T& object) {
    return schemaOf<T>().encode(object);
}

inline constexpr std::string_view kProjectSchema = "supple.project/1.0";

// The whole session as the canonical project document (members in the
// spec's order, objects keyed by ID in ID order).
json::Value projectToJson(const Session& s);
std::string serializeProject(const Session& s);  // canonical text + "\n"
// Decodes and validates (schema errors with paths, then invariants).
Result<Session> projectFromJson(const json::Value& doc);
Result<Session> parseProject(std::string_view text);

}  // namespace supple::model
