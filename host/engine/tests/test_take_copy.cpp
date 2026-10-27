// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include "supple/engine/app.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;

TEST_CASE("copying a take from history retains its opaque FX state after save and reopen") {
    model::EditTxn base(model::Session::create(), 1);
    model::Track channel;
    channel.parent = model::kMaster;
    const auto track = base.create(channel);
    base.modify<model::Track>(model::kMaster, [&](auto& m) { m.children.push_back(track); });
    auto empty = base.commit();
    REQUIRE(empty);

    model::EditTxn saved(*empty, 2);
    model::FxInstance fx;
    fx.plugin.id = "test.opaque";
    fx.state = "sha256:clipboard";
    const auto blob = std::make_shared<const model::Blob>(model::Blob{{1, 2, 3, 4}});
    saved.putBlob(*fx.state, blob);
    model::Take take;
    take.chain.push_back(saved.create(fx));
    const auto original = saved.create(take);
    model::Item item;
    item.track = track;
    item.takes = {original};
    item.activeTake = original;
    saved.create(item);
    auto source = saved.commit();
    REQUIRE(source);

    // Copy, undo to before the take/state existed, then paste the retained version.
    model::EditTxn pasted(*empty, 3);
    auto copy = copyTake(*source, pasted, original, take);
    REQUIRE(copy);
    item.takes = {*copy};
    item.activeTake = *copy;
    pasted.create(item);
    auto result = pasted.commit();
    REQUIRE(result);
    CHECK(result->blobs.contains(*fx.state));
    auto reopened = model::parseProject(model::serializeProject(*result));
    REQUIRE(reopened);
    const auto& copied = *reopened->takes.find(*copy);
    REQUIRE(copied.chain.size() == 1);
    CHECK(reopened->fx.find(copied.chain.front())->state == fx.state);
    const auto state = reopened->blobs.find(*fx.state);
    REQUIRE(state != reopened->blobs.end());
    CHECK(state->second->data == blob->data);
}
