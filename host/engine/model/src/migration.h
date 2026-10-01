#pragma once
#include "supple/model/schemas.h"

namespace supple::model {
Result<json::Value> migrateProject1(json::Value doc, std::vector<schema::Warning>* warnings);
}
