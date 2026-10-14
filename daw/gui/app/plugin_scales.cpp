// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "plugin_scales.h"

namespace supple::gui {

std::optional<views::EnvScale> PluginScales::operator()(const model::Envelope& env) {
    if (env.target.object.kind != model::Kind::Fx) return std::nullopt;
    const auto key = std::make_pair(env.target.object, env.target.param);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        std::shared_ptr<const params::DescribedParam> found;
        json::Value args = json::Value::object();
        args["fx"] = env.target.object.str();
        args["full"] = true;
        auto described = dispatcher_.execute(
            {"fx.describe", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
        if (described)
            if (const json::Value* ps = described->find("params"); ps && ps->isObject())
                if (const json::Value* entry = ps->find(env.target.param))
                    if (auto spec = params::fromDescription(env.target.param, *entry); spec)
                        found = std::shared_ptr<const params::DescribedParam>(std::move(*spec));
        it = cache_.emplace(key, std::move(found)).first;
    }
    if (!it->second) return std::nullopt;
    views::EnvScale scale;
    scale.kind = views::EnvScale::Kind::Plugin;
    scale.param = it->second;
    if (app_)
        if (auto* instance = app_->instanceOf(env.target.object))
            scale.normalizedInterpolation =
                instance->info().semantic || instance->paramsNormalized();
    return scale;
}

}  // namespace supple::gui
