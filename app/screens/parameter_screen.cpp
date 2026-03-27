#include "screens/parameter_screen.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include <algorithm>

using namespace ftxui;

namespace rtl::tui {

ParameterScreen::ParameterScreen(NodeInspector* inspector, ParameterManager* param_mgr)
    : inspector_(inspector), param_mgr_(param_mgr) {}

bool ParameterScreen::inputActive() const {
    return search_bar_.inputActive() || editing_;
}

ftxui::Component ParameterScreen::component() {
    auto renderer = Renderer([this] {
        std::lock_guard lock(mutex_);
        return vbox({
            hbox({
                renderNodeList() | size(WIDTH, EQUAL, 30) | border,
                renderParamPanel() | flex | border,
            }) | flex,
            renderStatusBar(),
        });
    });

    return CatchEvent(renderer, [this](Event event) {
        std::lock_guard lock(mutex_);

        // Edit mode handles all input
        if (editing_) {
            if (event == ftxui::Event::Escape) {
                editing_ = false;
                edit_error_.clear();
                return true;
            }
            if (event == ftxui::Event::Return) {
                // Commit the edit
                if (!cached_node_names_.empty()) {
                    int node_sel = node_list_.selected();
                    if (node_sel >= 0 && node_sel < (int)cached_node_names_.size()) {
                        param_mgr_->setParameter(
                            cached_node_names_[node_sel],
                            edit_param_name_,
                            edit_buffer_,
                            edit_param_type_,
                            [this](bool success, const std::string& msg) {
                                std::lock_guard lock(mutex_);
                                if (success) {
                                    editing_ = false;
                                    edit_error_.clear();
                                    status_message_ = "Parameter set successfully";
                                    status_time_ = std::chrono::steady_clock::now();
                                } else {
                                    edit_error_ = msg;
                                }
                            });
                    }
                }
                return true;
            }
            if (event == ftxui::Event::Backspace) {
                if (!edit_buffer_.empty()) edit_buffer_.pop_back();
                edit_error_.clear();
                return true;
            }
            if (event.is_character()) {
                edit_buffer_ += event.character();
                edit_error_.clear();
                return true;
            }
            return false;
        }

        // Search bar gets priority
        if (search_bar_.handleEvent(event)) {
            // Reset param selection when search changes
            param_list_ = ScrollableList{ScrollableList::Config{true}};
            return true;
        }
        if (search_bar_.inputActive()) return false;

        // Panel switching with Left/Right
        if (event == ftxui::Event::ArrowLeft) {
            active_panel_ = Panel::Nodes;
            return true;
        }
        if (event == ftxui::Event::ArrowRight) {
            active_panel_ = Panel::Params;
            return true;
        }
        if (event == ftxui::Event::Tab) {
            active_panel_ = (active_panel_ == Panel::Nodes) ? Panel::Params : Panel::Nodes;
            return true;
        }

        // Delegate navigation to active panel's scroll list
        if (active_panel_ == Panel::Nodes) {
            if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
                event == ftxui::Event::PageUp || event == ftxui::Event::PageDown ||
                event == ftxui::Event::Home || event == ftxui::Event::End ||
                event.is_mouse()) {
                bool consumed = node_list_.handleEvent(event);
                if (consumed) {
                    // Reset param list when switching nodes
                    param_list_ = ScrollableList{ScrollableList::Config{true}};
                }
                return consumed;
            }
        } else {
            if (event == ftxui::Event::ArrowUp || event == ftxui::Event::ArrowDown ||
                event == ftxui::Event::PageUp || event == ftxui::Event::PageDown ||
                event == ftxui::Event::Home || event == ftxui::Event::End ||
                event.is_mouse()) {
                return param_list_.handleEvent(event);
            }
        }

        // Enter: edit selected parameter
        if (event == ftxui::Event::Return && active_panel_ == Panel::Params) {
            if (!filtered_indices_.empty()) {
                int sel = param_list_.selected();
                if (sel >= 0 && sel < (int)filtered_indices_.size()) {
                    int pi = filtered_indices_[sel];
                    if (pi < (int)cached_params_.params.size()) {
                        const auto& param = cached_params_.params[pi];
                        if (param.read_only) {
                            status_message_ = "Parameter is read-only";
                            status_time_ = std::chrono::steady_clock::now();
                            return true;
                        }
                        // For booleans, toggle directly
                        if (param.type == rcl_interfaces::msg::ParameterType::PARAMETER_BOOL) {
                            std::string new_val = (param.value_str == "true") ? "false" : "true";
                            int node_sel = node_list_.selected();
                            if (node_sel >= 0 && node_sel < (int)cached_node_names_.size()) {
                                param_mgr_->setParameter(
                                    cached_node_names_[node_sel],
                                    param.name, new_val, param.type,
                                    [this](bool success, const std::string& msg) {
                                        std::lock_guard lock(mutex_);
                                        if (success) {
                                            status_message_ = "Boolean toggled";
                                            status_time_ = std::chrono::steady_clock::now();
                                        } else {
                                            status_message_ = "Failed: " + msg;
                                            status_time_ = std::chrono::steady_clock::now();
                                        }
                                    });
                            }
                            return true;
                        }
                        // Other types: enter edit mode
                        editing_ = true;
                        edit_buffer_ = param.value_str;
                        edit_param_name_ = param.name;
                        edit_param_type_ = param.type;
                        edit_error_.clear();
                        return true;
                    }
                }
            }
            return true;
        }

        // 'd' — dump parameters to YAML
        if (event.is_character() && event.character() == "d") {
            int node_sel = node_list_.selected();
            if (node_sel >= 0 && node_sel < (int)cached_node_names_.size()) {
                const auto& node_name = cached_node_names_[node_sel];
                // Sanitize node name for filename
                std::string filename = node_name;
                for (auto& c : filename) {
                    if (c == '/') c = '_';
                }
                if (!filename.empty() && filename[0] == '_') filename = filename.substr(1);
                filename += "_params.yaml";

                if (param_mgr_->dumpToYaml(node_name, filename)) {
                    status_message_ = "Dumped to " + filename;
                } else {
                    status_message_ = "Dump failed (no params loaded?)";
                }
                status_time_ = std::chrono::steady_clock::now();
            }
            return true;
        }

        // 'r' — force refresh
        if (event.is_character() && event.character() == "r") {
            int node_sel = node_list_.selected();
            if (node_sel >= 0 && node_sel < (int)cached_node_names_.size()) {
                param_mgr_->forceRefresh(cached_node_names_[node_sel]);
            }
            return true;
        }

        return false;
    });
}

void ParameterScreen::tick() {
    inspector_->refresh();
    auto nodes = inspector_->nodes();

    // Build sorted node name list
    std::vector<std::string> node_names;
    node_names.reserve(nodes.size());
    for (const auto& n : nodes) {
        node_names.push_back(n.full_name);
    }
    std::sort(node_names.begin(), node_names.end());

    // Request params for selected node
    NodeParameters params;
    {
        std::lock_guard lock(mutex_);
        cached_node_names_ = std::move(node_names);
        node_list_.setItemCount((int)cached_node_names_.size());

        int sel = node_list_.selected();
        if (sel >= 0 && sel < (int)cached_node_names_.size()) {
            param_mgr_->requestNodeParams(cached_node_names_[sel]);
            params = param_mgr_->getNodeParams(cached_node_names_[sel]);
        }
        cached_params_ = std::move(params);
    }

    // Prune stale nodes
    param_mgr_->pruneNodes(cached_node_names_);
}

std::vector<int> ParameterScreen::filteredParamIndices() const {
    std::vector<int> indices;
    const auto& query = search_bar_.query();
    for (int i = 0; i < (int)cached_params_.params.size(); ++i) {
        if (query.empty() ||
            cached_params_.params[i].name.find(query) != std::string::npos) {
            indices.push_back(i);
        }
    }
    return indices;
}

Element ParameterScreen::renderNodeList() {
    int selected = node_list_.selected();

    Elements rows;
    rows.push_back(text(" NODES") | bold);
    rows.push_back(separator());

    for (int i = 0; i < (int)cached_node_names_.size(); ++i) {
        bool is_sel = (i == selected && active_panel_ == Panel::Nodes);
        std::string prefix = is_sel ? " > " : "   ";
        auto row = text(prefix + cached_node_names_[i]);
        if (is_sel) {
            row = row | bold | inverted;
        } else if (i == selected) {
            row = row | bold;
        }
        rows.push_back(row);
    }

    if (cached_node_names_.empty()) {
        rows.push_back(text("  No nodes") | dim);
    }

    return vbox(std::move(rows));
}

Element ParameterScreen::renderParamPanel() {
    int node_sel = node_list_.selected();
    std::string node_name = (node_sel >= 0 && node_sel < (int)cached_node_names_.size())
        ? cached_node_names_[node_sel] : "";

    Elements rows;

    // Title
    rows.push_back(hbox({
        text(" PARAMETERS") | bold,
        text(node_name.empty() ? "" : " for " + node_name) | dim,
    }));

    // Search bar
    rows.push_back(search_bar_.render());
    rows.push_back(separator());

    // Status check
    if (node_name.empty()) {
        rows.push_back(text("  Select a node to view parameters") | dim);
        return vbox(std::move(rows));
    }

    if (cached_params_.loading && !cached_params_.loaded) {
        rows.push_back(text("  Loading parameters...") | dim);
        return vbox(std::move(rows));
    }

    if (!cached_params_.error.empty() && !cached_params_.loaded) {
        rows.push_back(text("  " + cached_params_.error) | color(Color::Red));
        return vbox(std::move(rows));
    }

    // Update filtered indices
    filtered_indices_ = filteredParamIndices();
    param_list_.setItemCount((int)filtered_indices_.size());

    if (filtered_indices_.empty()) {
        if (cached_params_.params.empty()) {
            rows.push_back(text("  No parameters") | dim);
        } else {
            rows.push_back(text("  No parameters match search") | dim);
        }
        return vbox(std::move(rows)) | flex;
    }

    // Header
    rows.push_back(hbox({
        text("   NAME") | bold | size(WIDTH, EQUAL, 35),
        text("TYPE") | bold | size(WIDTH, EQUAL, 10),
        text("VALUE") | bold | size(WIDTH, EQUAL, 30),
        text("R/O") | bold | size(WIDTH, EQUAL, 5),
    }));
    rows.push_back(separator());

    // Parameter rows
    int param_sel = param_list_.selected();
    for (int fi = 0; fi < (int)filtered_indices_.size(); ++fi) {
        int pi = filtered_indices_[fi];
        const auto& p = cached_params_.params[pi];

        bool is_sel = (fi == param_sel && active_panel_ == Panel::Params);
        std::string prefix = is_sel ? " > " : "   ";

        // Value display — show edit buffer if editing this param
        std::string value_display = p.value_str;
        bool is_editing_this = editing_ && p.name == edit_param_name_ && is_sel;
        if (is_editing_this) {
            value_display = edit_buffer_ + "_";
        }

        Color type_color = Color::GrayLight;
        if (p.type_name == "bool") type_color = Color::Magenta;
        else if (p.type_name == "int") type_color = Color::Blue;
        else if (p.type_name == "double") type_color = Color::Green;
        else if (p.type_name == "string") type_color = Color::Yellow;

        auto row = hbox({
            text(prefix + p.name) | size(WIDTH, EQUAL, 35),
            text(p.type_name) | color(type_color) | size(WIDTH, EQUAL, 10),
            text(value_display) | (is_editing_this ? color(Color::Cyan) : nothing) | size(WIDTH, EQUAL, 30),
            text(p.read_only ? "[RO]" : "") | dim | size(WIDTH, EQUAL, 5),
        });

        if (is_sel) {
            row = row | bold | inverted;
        } else if (fi == param_sel) {
            row = row | bold;
        }

        rows.push_back(row);
    }

    // Detail section for selected parameter
    auto detail = renderParamDetail();

    return vbox({
        vbox(std::move(rows)) | flex,
        detail,
    });
}

Element ParameterScreen::renderParamDetail() {
    int param_sel = param_list_.selected();
    if (param_sel < 0 || param_sel >= (int)filtered_indices_.size()) {
        return text("");
    }

    int pi = filtered_indices_[param_sel];
    if (pi >= (int)cached_params_.params.size()) return text("");

    const auto& p = cached_params_.params[pi];

    Elements detail_rows;
    detail_rows.push_back(separator());
    detail_rows.push_back(text(" Detail") | bold);

    detail_rows.push_back(hbox({
        text("  Type: ") | dim,
        text(p.type_name),
        text("  Value: ") | dim,
        text(p.value_str),
    }));

    if (!p.description.empty()) {
        detail_rows.push_back(hbox({
            text("  Description: ") | dim,
            text(p.description),
        }));
    }

    if (p.has_integer_range) {
        detail_rows.push_back(hbox({
            text("  Range: ") | dim,
            text("[" + std::to_string(p.int_range_min) + ", " +
                 std::to_string(p.int_range_max) + "]"),
            p.int_range_step > 0
                ? text("  step: " + std::to_string(p.int_range_step)) | dim
                : text(""),
        }));
    }

    if (p.has_float_range) {
        auto fmt = [](double v) {
            std::ostringstream oss;
            oss << v;
            return oss.str();
        };
        detail_rows.push_back(hbox({
            text("  Range: ") | dim,
            text("[" + fmt(p.float_range_min) + ", " + fmt(p.float_range_max) + "]"),
            p.float_range_step > 0
                ? text("  step: " + fmt(p.float_range_step)) | dim
                : text(""),
        }));
    }

    detail_rows.push_back(hbox({
        text("  Read-only: ") | dim,
        text(p.read_only ? "Yes" : "No") | (p.read_only ? color(Color::Red) : color(Color::Green)),
    }));

    // Show edit error if any
    if (editing_ && !edit_error_.empty() && p.name == edit_param_name_) {
        detail_rows.push_back(
            text("  Error: " + edit_error_) | color(Color::Red));
    }

    return vbox(std::move(detail_rows));
}

Element ParameterScreen::renderStatusBar() {
    // Clear status message after 5 seconds
    std::string status;
    if (!status_message_.empty()) {
        auto elapsed = std::chrono::steady_clock::now() - status_time_;
        if (elapsed < std::chrono::seconds(5)) {
            status = status_message_;
        } else {
            status_message_.clear();
        }
    }

    Elements bar;
    if (editing_) {
        bar.push_back(text(" [Enter] Confirm  [Esc] Cancel") | dim);
    } else {
        bar.push_back(text(" [" + std::string(active_panel_ == Panel::Nodes ? ">" : " ") +
                           "Nodes] [" +
                           std::string(active_panel_ == Panel::Params ? ">" : " ") +
                           "Params]  ") | dim);
        bar.push_back(text("[Tab/\u2190\u2192] Panel  [\u2191\u2193] Select  [Enter] Edit  [d] Dump  [/] Search  [r] Refresh") | dim);
    }

    if (!status.empty()) {
        bar.push_back(text("  " + status) | color(Color::Cyan));
    }

    // Show loading indicator
    if (cached_params_.loading) {
        bar.push_back(text("  [loading...]") | color(Color::Yellow));
    }

    return hbox(std::move(bar));
}

}  // namespace rtl::tui
