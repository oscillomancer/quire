#include "agent_panel.h"

#include <cstdio>

#include "supple/json/write.h"
#include "supple/model/session.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

// A rounded card behind its children (messages, listening requests).
class Card : public ui::Node {
  public:
    Card(std::string id, std::string token, float alpha = 1.0f)
        : Node(std::move(id)), token_(std::move(token)), alpha_(alpha) {
        layout.dir = ui::Layout::Dir::Column;
        layout.height = ui::SizeRule::content();
        layout.padding = ui::Insets::xy(10, 6);
        layout.gap = 4;
    }
    void paint(ui::Canvas& c) override {
        c.fillRound(bounds(), c.theme().radius("m"), c.theme().color(token_).withAlpha(alpha_));
    }

  private:
    std::string token_;
    float alpha_;
};

class PanelFrame : public ui::Node {
  public:
    using Node::Node;
    void paint(ui::Canvas& c) override {
        c.fillRect(bounds(), c.theme().color("panel"));
        c.fillRect({bounds().x, bounds().y, 1, bounds().h}, c.theme().color("rule"));
    }
};

std::string shortJson(const json::Value& v, std::size_t max = 90) {
    std::string s = json::writeCompact(v);
    return s.size() > max ? s.substr(0, max - 1) + "…" : s;
}

}  // namespace

AgentPanel::AgentPanel(ui::Root& root, engine::EngineApp& app, const cmd::Registry& registry,
                       cmd::Dispatcher& dispatcher, Post post, std::filesystem::path configFile,
                       std::filesystem::path skillDir)
    : root_(root),
      app_(app),
      registry_(registry),
      dispatcher_(dispatcher),
      post_(std::move(post)),
      configFile_(std::move(configFile)),
      skillDir_(std::move(skillDir)) {
    if (auto loaded = agent::Config::load(configFile_))
        config_ = std::move(*loaded);
    else
        std::fprintf(stderr, "agent: %s\n", loaded.error().toString().c_str());
}

AgentPanel::~AgentPanel() = default;

std::unique_ptr<ui::Node> AgentPanel::build() {
    auto panel = std::make_unique<PanelFrame>("agent");
    panel_ = panel.get();
    panel->visible = false;
    panel->layout.dir = ui::Layout::Dir::Column;
    panel->layout.width = ui::SizeRule::fixed(400);
    panel->layout.padding = ui::Insets::all(10);
    panel->layout.gap = 8;

    auto& header = panel->emplace<ui::Node>("agent.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(28);
    header.layout.gap = 8;
    header.layout.align = ui::Layout::Align::Center;
    header.emplace<ui::Label>("agent.title", "Agent", ui::Font::UiBold, "body");
    model_ = &header.emplace<ui::Label>("agent.model", "", ui::Font::Mono, "caption", "ink-quiet");
    model_->layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("agent.settings", "Settings", [this] { openSettings(); });

    transcript_ = &panel->emplace<ui::ScrollView>("agent.transcript");
    transcript_->stickToEnd = true;
    transcript_->content().layout.gap = 8;
    transcript_->content().layout.padding = ui::Insets{0, 0, 4, 0};

    auto& review = panel->emplace<Card>("agent.review", "raised");
    review_ = &review;
    review.visible = false;
    reviewTitle_ = &review.emplace<ui::Label>("agent.review.title", "", ui::Font::UiBold, "small");
    reviewText_ =
        &review.emplace<ui::TextBlock>("agent.review.diff", "", ui::Font::Mono, "caption", "ink");
    auto& buttons = review.emplace<ui::Node>("agent.review.buttons");
    buttons.layout.dir = ui::Layout::Dir::Row;
    buttons.layout.height = ui::SizeRule::fixed(28);
    buttons.layout.gap = 6;
    auto audition = [this](bool agentBranch) {
        if (!session_) return;
        if (auto r = session_->audition(agentBranch); !r)
            append(std::make_unique<ui::TextBlock>("agent.err." + std::to_string(serial_++),
                                                   r.error().message, ui::Font::Ui, "caption",
                                                   "clip"));
    };
    buttons.emplace<ui::Button>("agent.yours", "Hear yours", [audition] { audition(false); });
    buttons.emplace<ui::Button>("agent.theirs", "Hear agent's", [audition] { audition(true); });
    buttons.emplace<ui::Node>("agent.review.gap");
    buttons.emplace<ui::Button>("agent.accept", "Accept", [this] {
        if (!session_) return;
        auto r = session_->accept();
        append(std::make_unique<ui::TextBlock>(
            "agent.note." + std::to_string(serial_++),
            r ? "Accepted: merged into your version." : r.error().message, ui::Font::Ui, "caption",
            r ? "ink-quiet" : "clip"));
        refreshReview();
    });
    buttons.emplace<ui::Button>("agent.reject", "Reject", [this] {
        if (!session_) return;
        auto r = session_->reject();
        append(std::make_unique<ui::TextBlock>(
            "agent.note." + std::to_string(serial_++),
            r ? "Rejected: the agent's changes were discarded." : r.error().message, ui::Font::Ui,
            "caption", r ? "ink-quiet" : "clip"));
        refreshReview();
    });

    auto& inputRow = panel->emplace<ui::Node>("agent.input.row");
    inputRow.layout.dir = ui::Layout::Dir::Row;
    inputRow.layout.height = ui::SizeRule::fixed(28);
    inputRow.layout.gap = 6;
    input_ = &inputRow.emplace<ui::TextField>("agent.input", "", "Ask the agent…");
    input_->layout.width = ui::SizeRule::fraction();
    input_->onSubmit = [this](const std::string& text) {
        if (!text.empty()) send(text);
    };
    send_ = &inputRow.emplace<ui::Button>("agent.send", "Send", [this] {
        if (session_ && session_->busy())
            session_->cancel();
        else if (!input_->text().empty())
            send(input_->text());
    });
    model_->setText(config_.active() ? config_.active()->model : "no endpoint");
    return panel;
}

bool AgentPanel::visible() const { return panel_ && panel_->visible; }

void AgentPanel::toggle() {
    if (!panel_) return;
    panel_->visible = !panel_->visible;
    if (!panel_->visible) root_.releaseWithin(panel_);
    root_.relayout();
    root_.damageAll();
    if (panel_->visible) root_.focus(input_);
}

void AgentPanel::setVisible(bool visible) {
    if (panel_ && panel_->visible != visible) toggle();
}

void AgentPanel::append(std::unique_ptr<ui::Node> node) {
    transcript_->content().add(std::move(node));
    transcript_->invalidate();
}

void AgentPanel::setBusy(bool busy) {
    send_->setText(busy ? "Stop" : "Send");
    review_->visible = !busy && session_ && !session_->branch().empty();
    root_.relayout();
}

void AgentPanel::ensureSession() {
    if (session_ || !config_.active()) return;
    agent::Options options;
    options.skillDir = skillDir_;
    session_ =
        std::make_unique<agent::Session>(*config_.active(), registry_, dispatcher_, post_, options);
    agent::Events& ev = session_->events;
    ev.text = [this](std::string_view d) {
        if (!speaking_) {
            auto block =
                std::make_unique<ui::TextBlock>("agent.say." + std::to_string(serial_++), "");
            speaking_ = block.get();
            append(std::move(block));
        }
        speaking_->append(d);
        transcript_->invalidate();
    };
    ev.turn = [this](const agent::Turn&) { speaking_ = nullptr; };
    ev.toolStarted = [this](const agent::ToolCall& c) {
        std::string label = "> " + c.name;
        if (c.name == "exec" && agent::at(c.input, "command").isString())
            label += " " + agent::at(c.input, "command").asString() + " " +
                     shortJson(agent::at(c.input, "args"), 60);
        else
            label += " " + shortJson(c.input, 70);
        auto row = std::make_unique<ui::TextBlock>("agent.tool." + std::to_string(serial_++), label,
                                                   ui::Font::Mono, "caption", "ink-quiet");
        tools_[c.id] = row.get();
        append(std::move(row));
        speaking_ = nullptr;
    };
    ev.toolFinished = [this](const agent::ToolCall& c, const agent::ToolResult& r) {
        if (auto it = tools_.find(c.id); it != tools_.end()) {
            std::string note = r.content.substr(0, 80);
            std::replace(note.begin(), note.end(), '\n', ' ');
            it->second->setText(it->second->text() + (r.isError ? "  -- error: " : "  -- ok: ") +
                                note);
        }
    };
    ev.listen = [this](const json::Value& card) {
        auto box =
            std::make_unique<Card>("agent.listen." + std::to_string(serial_++), "accent", 0.18f);
        std::string what =
            "Listen: " +
            (agent::at(card, "what").isString() ? agent::at(card, "what").asString() : "");
        const json::Value& from = agent::at(card, "start_bar");
        const json::Value& to = agent::at(card, "end_bar");
        if (from.isNumber())
            what += "  (bars " + std::to_string(from.asInt()) +
                    (to.isNumber() ? "–" + std::to_string(to.asInt()) : "") + ")";
        box->emplace<ui::TextBlock>(box->id() + ".text", what, ui::Font::Ui, "small", "ink");
        const int bar = from.isNumber() ? static_cast<int>(from.asInt()) : 1;
        auto& row = box->emplace<ui::Node>(box->id() + ".buttons");
        row.layout.dir = ui::Layout::Dir::Row;
        row.layout.height = ui::SizeRule::fixed(28);
        row.layout.gap = 6;
        row.emplace<ui::Button>(
            box->id() + ".play", "Play from bar " + std::to_string(bar), [this, bar] {
                json::Value seek = json::Value::object();
                seek["bar"] = bar;
                (void)dispatcher_.execute(
                    {"transport.seek", std::move(seek), {cmd::Origin::Type::Ui, "gui", {}}});
                (void)dispatcher_.execute(
                    {"transport.play", json::Value::object(), {cmd::Origin::Type::Ui, "gui", {}}});
            });
        append(std::move(box));
    };
    ev.error = [this](const std::string& message) {
        append(std::make_unique<ui::TextBlock>("agent.err." + std::to_string(serial_++), message,
                                               ui::Font::Ui, "caption", "clip"));
    };
    ev.idle = [this] {
        speaking_ = nullptr;
        setBusy(false);
        refreshReview();
    };
}

void AgentPanel::send(const std::string& prompt) {
    if (!config_.active()) {
        openSettings();
        return;
    }
    ensureSession();
    if (!session_ || session_->busy()) return;
    const std::string submitted = prompt;
    auto card = std::make_unique<Card>("agent.you." + std::to_string(serial_++), "selection", 0.7f);
    card->emplace<ui::TextBlock>(card->id() + ".text", submitted);
    append(std::move(card));
    input_->setText("");
    speaking_ = nullptr;
    if (session_->send(submitted)) setBusy(true);
}

std::string AgentPanel::describeChange(const json::Value& change) const {
    const std::string op = agent::at(change, "op").asString();
    const std::string idText = agent::at(change, "id").asString();
    const auto id = model::Id::parse(idText);
    // Added and changed objects live in the agent's head; removed ones in the base's.
    const auto& versions = app_.versions();
    std::shared_ptr<const model::Session> s =
        op == "remove" ? versions.head(session_->base()) : versions.head(session_->branch());
    std::string what = idText;
    if (id && s) {
        auto trackName = [&](model::Id t) {
            const model::Track* tr = s->tracks.find(t);
            return tr ? "\"" +
                            (tr->name.empty() ? std::string(model::stripTypeName(tr->type))
                                              : tr->name) +
                            "\""
                      : t.str();
        };
        switch (id->kind) {
            case model::Kind::Track:
                if (const model::Track* t = s->tracks.find(*id))
                    what = std::string(model::stripTypeName(t->type)) + " " + trackName(*id);
                break;
            case model::Kind::Item:
                if (const model::Item* it = s->items.find(*id)) {
                    char at[32];
                    std::snprintf(at, sizeof at, "%.1f s", model::itemSpan(*s, *it).startSec);
                    what = "item on " + trackName(it->track) + " at " + at;
                }
                break;
            case model::Kind::Fx:
                if (const model::FxInstance* fx = s->fx.find(*id))
                    what = (fx->plugin.name.empty() ? fx->plugin.id : fx->plugin.name) + " (" +
                           idText + ")";
                break;
            case model::Kind::Marker:
                if (const auto* m = s->markers.find(*id)) what = "marker \"" + m->name + "\"";
                break;
            default:
                break;
        }
    }
    std::string fields;
    for (const json::Value& f : agent::at(change, "fields").asArray())
        fields += (fields.empty() ? "" : ", ") + f.asString();
    const char* mark = op == "add" ? "+ " : op == "remove" ? "- " : "~ ";
    return mark + what + (fields.empty() || op != "update" ? "" : ": " + fields);
}

void AgentPanel::refreshReview() {
    const bool show = session_ && !session_->busy() && !session_->branch().empty();
    review_->visible = show;
    if (show) {
        reviewTitle_->setText("Changes on " + session_->branch());
        std::string text;
        if (auto diff = session_->diff()) {
            const json::Value& changes = agent::at(*diff, "changes");
            for (const json::Value& c : changes.isArray() ? changes.asArray() : json::Array{}) {
                // A parent's "children" list changes whenever a child is added
                // or removed; the child's own line says it.
                const json::Value& fields = agent::at(c, "fields");
                if (agent::at(c, "op").asString() == "update" && fields.isArray() &&
                    fields.asArray().size() == 1 && fields.asArray()[0].asString() == "children")
                    continue;
                text += (text.empty() ? "" : "\n") + describeChange(c);
            }
        }
        reviewText_->setText(text.empty() ? "No changes." : text);
    }
    root_.relayout();
    root_.damageAll();
}

void AgentPanel::openSettings() {
    agent::Endpoint ep =
        config_.active() ? *config_.active() : agent::Endpoint{agent::Endpoint::Kind::Anthropic,
                                                               "Anthropic",
                                                               "https://api.anthropic.com",
                                                               "",
                                                               "",
                                                               4096};
    auto form = std::make_unique<ui::Node>("agent.settings.form");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(460);
    form->layout.height = ui::SizeRule::fixed(372);
    form->layout.padding = ui::Insets::all(14);
    form->layout.gap = 6;
    form->emplace<ui::Label>("agent.settings.title", "Agent endpoint", ui::Font::UiBold, "body");
    auto kind = std::make_shared<agent::Endpoint::Kind>(ep.kind);
    auto& kinds = form->emplace<ui::Node>("agent.settings.kinds");
    kinds.layout.dir = ui::Layout::Dir::Row;
    kinds.layout.height = ui::SizeRule::fixed(28);
    kinds.layout.gap = 6;
    auto field = [&](const char* id, const char* label,
                     const std::string& value) -> ui::TextField& {
        form->emplace<ui::Label>(std::string(id) + ".label", label, ui::Font::Ui, "caption",
                                 "ink-quiet");
        return form->emplace<ui::TextField>(id, value);
    };
    auto& name = field("agent.settings.name", "Name", ep.name);
    auto& url = field("agent.settings.url", "Base URL", ep.baseUrl);
    auto& model = field("agent.settings.model", "Model", ep.model);
    auto& key = field("agent.settings.key",
                      "API key (stored 0600 in your config, never in a project)", ep.apiKey);
    key.secret = true;
    kinds.emplace<ui::Button>("agent.settings.anthropic", "Anthropic", [kind, &url] {
        *kind = agent::Endpoint::Kind::Anthropic;
        if (url.text().empty() || url.text().find("127.0.0.1") != std::string::npos)
            url.setText("https://api.anthropic.com");
    });
    kinds.emplace<ui::Button>("agent.settings.openai", "OpenAI-compatible", [kind, &url] {
        *kind = agent::Endpoint::Kind::OpenAI;
        if (url.text().empty() || url.text().find("anthropic") != std::string::npos)
            url.setText("http://127.0.0.1:8080/v1");
    });
    auto save = [this, kind, &name, &url, &model, &key,
                 maxTokens = ep.maxTokens](const std::string&) {
        agent::Endpoint e{*kind, name.text(), url.text(), model.text(), key.text(), maxTokens};
        if (config_.current >= config_.endpoints.size()) {
            config_.current = config_.endpoints.size();
            config_.endpoints.push_back(e);
        } else
            config_.endpoints[config_.current] = e;
        ui::Root& root = root_;  // closePopup destroys this lambda: copy what follows first
        AgentPanel* self = this;
        root.closePopup();
        if (auto saved = self->config_.save(self->configFile_); !saved)
            self->append(std::make_unique<ui::TextBlock>(
                "agent.err." + std::to_string(self->serial_++), saved.error().message, ui::Font::Ui,
                "caption", "clip"));
        // The next turn uses it; a pending proposal and the conversation stay.
        if (self->session_ && self->config_.active())
            self->session_->setEndpoint(*self->config_.active());
        self->model_->setText(self->config_.active()->model);
    };
    for (ui::TextField* f : {&name, &url, &model, &key}) f->onSubmit = save;
    form->emplace<ui::Button>("agent.settings.save", "Save", [save] { save(""); });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
}

}  // namespace supple::gui
