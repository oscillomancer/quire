// Automation lanes for plugin parameters (P5.14): a parameter's range, curve
// and units come from the effect's description (fx.describe). Cached per
// effect and parameter; a parameter the plugin does not describe is
// remembered as having no lane.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "supple/cmd/bus.h"
#include "supple/views/envelope_lane.h"

namespace supple::gui {

class PluginScales {
  public:
    explicit PluginScales(cmd::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
    std::optional<views::EnvScale> operator()(const model::Envelope& env);

  private:
    cmd::Dispatcher& dispatcher_;
    std::map<std::pair<model::Id, std::string>, std::shared_ptr<const params::DescribedParam>>
        cache_;
};

}  // namespace supple::gui
