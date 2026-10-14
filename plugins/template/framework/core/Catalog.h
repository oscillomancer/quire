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
// finds. An ID is permanent once shipped, as a product's is. CLAP lets a
// host call the factory from any thread, so the module calls the catalog
// with catalogMutex() held: one call at a time, from whichever thread.
#pragma once

#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "core/Definition.h"

namespace supple {

class Catalog {
  public:
    virtual ~Catalog() = default;
    // The plugins offered now, in a stable order. Any thread but the audio
    // thread, one call at a time.
    virtual std::vector<DefinitionPtr> list() = 0;
    // The plugin with this ID as it is now, or null. By default the one
    // list() has. As list().
    virtual DefinitionPtr find(std::string_view id);
};

// The module's catalog, made on first use and kept for the life of the
// process.
Catalog& catalog();
// Held around every use of the catalog and of what the factories made of
// it (their lists of descriptors).
std::mutex& catalogMutex();

}  // namespace supple

#if SUPPLE_PRODUCT_RUNTIME
namespace product {
// A run-time product's catalog, made once.
std::unique_ptr<supple::Catalog> makeCatalog();
}  // namespace product
#endif
