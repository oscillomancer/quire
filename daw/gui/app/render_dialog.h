// The render dialog (docs/specs/gui.md §4.8): a front end to render.mix,
// render.stems, render.regions and render.dry-run. Every control maps to a
// schema field; the file-name pattern gets a live dry-run preview, re-run on
// every edit (coalesced per frame). Renders run as jobs on the engine's
// workers; the dialog polls job.get and lists the result files.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "supple/engine/app.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Root;
class Button;
class Label;
class TextBlock;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

class RenderDialog {
  public:
    RenderDialog(engine::EngineApp&, cmd::Dispatcher& dispatcher, ui::Root& root);
    ~RenderDialog();

    void open();
    void close();
    bool isOpen() const { return page_ != nullptr; }

    // Frame pass: debounced preview refresh and job polling.
    void tick();
    // For tests: 0 runs the preview on the next tick without a wait.
    void setDebounceMs(int ms) { debounceMs_ = ms; }
    void refreshPreview();  // run render.dry-run now

    enum class Source { Master, Stems, Regions };
    void setSource(Source source);
    Source source() const { return source_; }
    // add_to_project: the rendered files become items on new tracks (engine).
    void setAddToProject(bool on);
    bool addToProject() const { return addToProject_; }
    // The pattern currently in the field ("" when empty).
    const std::string& pattern() const;
    const std::string& preview() const { return previewText_; }
    const std::string& statusText() const;
    // Start a render as a job; the id is empty when the command failed.
    const std::string& job() const { return job_; }
    void run();
    std::vector<std::string> resultFiles() const { return resultFiles_; }
    void setField(const std::string& id, const std::string& text);  // tests/scripts

  private:
    std::unique_ptr<ui::Node> buildPage();
    json::Value args(bool forJob) const;
    json::Value previewArgs() const;
    void markDirty();
    void setStatus(std::string text);
    void pollJob();

    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    Source source_ = Source::Master;
    bool addToProject_ = false;
    std::string pattern_, start_, end_, tail_, sampleRate_, bitDepth_, channels_, normalize_,
        target_, limit_, mode_, tap_;
    std::string previewText_, statusText_, job_;
    std::vector<std::string> resultFiles_;
    bool dirty_ = true;
    int debounceMs_ = 40;
    std::uint64_t lastEditMs_ = 0;

    ui::Node* page_ = nullptr;
    ui::Label* sourceLabel_ = nullptr;
    ui::Label* status_ = nullptr;
    ui::TextBlock* previewBlock_ = nullptr;
    ui::TextBlock* resultsBlock_ = nullptr;
    ui::Node* stemsRow_ = nullptr;
    ui::Label* modeLabel_ = nullptr;
    ui::Label* tapLabel_ = nullptr;
    ui::TextField* modeField_ = nullptr;
    ui::TextField* tapField_ = nullptr;
    ui::Button* addButton_ = nullptr;
};

}  // namespace supple::gui
