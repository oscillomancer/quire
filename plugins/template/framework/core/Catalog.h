// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The plugins a module offers. A product with tables offers one, its own
// Definition. A run-time product (PRODUCT_RUNTIME ON) offers whatever it
// finds when a host asks -- a plugin for each script in a folder, say -- by
// defining, in one of its sources:
//
//   std::unique_ptr<supple::Catalog> product::makeCatalog() { ... }
//
// Hosts list the catalog through the CLAP plugin factory (and the VST3
// wrapper through it), and make each instance from the Definition its ID
// finds. An ID is permanent once shipped, as a product's is.
#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "core/Definition.h"

namespace supple {

class Catalog {
  public:
    virtual ~Catalog() = default;
    // Main thread: the plugins offered now, in a stable order.
    virtual std::vector<DefinitionPtr> list() = 0;
    // Main thread: the plugin with this ID as it is now, or null. By default
    // the one list() has.
    virtual DefinitionPtr find(std::string_view id);
};

// The module's catalog, made on first use and kept for the life of the
// process.
Catalog& catalog();

}  // namespace supple

#if SUPPLE_PRODUCT_RUNTIME
namespace product {
// A run-time product's catalog, made once.
std::unique_ptr<supple::Catalog> makeCatalog();
}  // namespace product
#endif
