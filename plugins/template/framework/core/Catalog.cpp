// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "core/Catalog.h"

namespace supple {

DefinitionPtr Catalog::find(std::string_view id) {
    for (DefinitionPtr& d : list())
        if (d->info().id == id) return d;
    return nullptr;
}

namespace {
// A product with tables: its one plugin.
class ProductCatalog final : public Catalog {
  public:
    std::vector<DefinitionPtr> list() override { return {productDefinition()}; }
};
}  // namespace

Catalog& catalog() {
#if SUPPLE_PRODUCT_RUNTIME
    static const std::unique_ptr<Catalog> instance = product::makeCatalog();
#else
    static const std::unique_ptr<Catalog> instance = std::make_unique<ProductCatalog>();
#endif
    return *instance;
}

}  // namespace supple
