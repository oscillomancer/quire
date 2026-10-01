// The mixer (docs/specs/gui.md §4.4), docked under the arrange view: strips
// in track order grouped by type — channels, busses, masters — each with its
// type colour, FX slots (click: the plug-in's editor), sends, input/arm/
// monitor on channels, pan, fader and meter, mute and solo; Console 9 as a
// project toggle in the header. Every change is a command; a fader or pan
// drag is one undo step.
#pragma once

#include <string>
#include <vector>

#include "app_actions.h"
#include "meter_feed.h"
#include "supple/ui/root.h"

namespace supple::ui {
class Label;
class Fader;
class PanControl;
class Toggle;
class Button;
class Meter;
}  // namespace supple::ui

namespace supple::gui {

class Mixer {
  public:
    Mixer(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher, engine::EngineApp& app,
          MeterFeed& meters);
    ~Mixer();
    std::unique_ptr<ui::Node> build();  // the docked panel (hidden until shown)
    void toggle();
    void setVisible(bool visible);
    bool visible() const;
    void sync();  // every loop pass

    // Tests.
    struct Strip {
        std::string track, type;
        ui::Node* node = nullptr;
        ui::Label* name = nullptr;
        ui::Fader* fader = nullptr;
        ui::PanControl* pan = nullptr;
        ui::Toggle *mute = nullptr, *solo = nullptr, *arm = nullptr;
        ui::Button* monitor = nullptr;
        ui::Label* value = nullptr;
        ui::Meter* meter = nullptr;
    };
    const std::vector<Strip>& strips() const { return strips_; }

  private:
    void rebuild();
    void update();
    std::string structure() const;
    Result<json::Value> set(const std::string& track, const char* field, json::Value value,
                            const std::string& gesture = {});
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    MeterFeed& meters_;
    ui::Node* panel_ = nullptr;
    ui::Node* row_ = nullptr;  // the strips' container
    ui::Toggle* console9_ = nullptr;
    std::vector<Strip> strips_;
    std::string structure_;
    std::uint64_t version_ = ~0ull;
    std::uint64_t gesture_ = 0;  // a serial per drag: separate drags are separate undo steps
};

}  // namespace supple::gui
