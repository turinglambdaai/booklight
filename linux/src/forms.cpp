// Forms pane implementation — see forms.hpp.
#include "forms.hpp"

#include <algorithm>

#include "i18n.h"

namespace pdfgist::forms {
namespace {

// g_signal_connect is a GLib macro; route through the real function (same
// pattern as main.cpp / pdfview.cpp).
gulong hook(gpointer instance, gchar const* detailed_signal, auto&& handler,
            gpointer data, GClosureNotify destroy_data = nullptr,
            GConnectFlags flags = static_cast<GConnectFlags>(0)) {
  return g_signal_connect_data(instance, detailed_signal,
                               reinterpret_cast<GCallback>(
                                   std::forward<decltype(handler)>(handler)),
                               data, destroy_data, flags);
}

// Row index carried on each control so one handler can serve every row.
gint row_index(gpointer widget) {
  return GPOINTER_TO_INT(
      g_object_get_data(G_OBJECT(widget), "pdfgist-form-row"));
}

}  // namespace

FormsPane::FormsPane(PaneContext ctx) : ctx_(std::move(ctx)) {
  root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  auto* header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(header, 8);
  gtk_widget_set_margin_bottom(header, 4);
  gtk_widget_set_margin_start(header, 8);
  gtk_widget_set_margin_end(header, 8);
  auto* pill = gtk_button_new_with_label(Tr("ui.forms.export").c_str());
  gtk_widget_add_css_class(pill, "suggested-action");
  gtk_widget_set_halign(pill, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header), pill);
  gtk_box_append(GTK_BOX(root_), header);
  export_ = GTK_BUTTON(pill);

  auto* scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  list_ = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(list_, GTK_SELECTION_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                GTK_WIDGET(list_));
  gtk_box_append(GTK_BOX(root_), scroll);

  status_ = GTK_LABEL(gtk_label_new(nullptr));
  gtk_label_set_wrap(status_, TRUE);
  gtk_widget_set_margin_top(GTK_WIDGET(status_), 4);
  gtk_widget_set_margin_bottom(GTK_WIDGET(status_), 10);
  gtk_widget_set_margin_start(GTK_WIDGET(status_), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(status_), 12);
  gtk_widget_set_halign(GTK_WIDGET(status_), GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(root_), GTK_WIDGET(status_));

  hook(export_, "clicked",
       +[](GtkButton*, gpointer user_data) {
         static_cast<FormsPane*>(user_data)->export_filled();
       },
       this);

  refresh();
}

void FormsPane::set_status(std::string const& text, bool is_error) {
  gtk_label_set_text(status_, text.c_str());
  if (is_error) {
    gtk_widget_add_css_class(GTK_WIDGET(status_), "error");
    gtk_widget_remove_css_class(GTK_WIDGET(status_), "dim-label");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(status_), "error");
    gtk_widget_add_css_class(GTK_WIDGET(status_), "dim-label");
  }
}

void FormsPane::refresh() {
  rows_.clear();
  gtk_list_box_remove_all(list_);

  auto* tab = ctx_.active_tab();
  if (tab == nullptr) {
    set_status(Tr("ui.need.document"), false);
    gtk_widget_set_sensitive(GTK_WIDGET(export_), FALSE);
    return;
  }
  auto fields = tab->document()->form_fields();
  if (fields.empty()) {
    set_status(Tr("ui.forms.empty"), false);
    gtk_widget_set_sensitive(GTK_WIDGET(export_), FALSE);
    return;
  }
  set_status(Trf("ui.forms.count", std::to_string(fields.size())), false);
  gtk_widget_set_sensitive(GTK_WIDGET(export_), TRUE);

  populating_ = true;
  for (auto& field : fields) {
    auto* row = gtk_list_box_row_new();
    auto* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(card, 6);
    gtk_widget_set_margin_bottom(card, 6);
    gtk_widget_set_margin_start(card, 8);
    gtk_widget_set_margin_end(card, 8);

    auto* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    char const* type_key = field.kind == pdfium::FormFieldInfo::Kind::Check
                               ? "ui.forms.type-check"
                               : field.kind ==
                                         pdfium::FormFieldInfo::Kind::Choice
                                     ? "ui.forms.type-choice"
                                     : "ui.forms.type-text";
    auto* chip = gtk_label_new(Tr(type_key).c_str());
    gtk_widget_add_css_class(chip, "caption");
    gtk_widget_add_css_class(chip, "dim-label");
    gtk_box_append(GTK_BOX(head), chip);
    auto* name = gtk_label_new(field.name.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_box_append(GTK_BOX(head), name);
    gtk_box_append(GTK_BOX(card), head);

    FieldRow entry_row;
    entry_row.field = field;
    entry_row.desired = field.value;
    GtkWidget* control = nullptr;
    switch (field.kind) {
      case pdfium::FormFieldInfo::Kind::Check: {
        auto* check = gtk_check_button_new_with_label(
            Tr("ui.forms.type-check").c_str());
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), field.checked);
        entry_row.check = GTK_CHECK_BUTTON(check);
        control = check;
        break;
      }
      case pdfium::FormFieldInfo::Kind::Choice: {
        auto* entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), field.value.c_str());
        gtk_entry_set_placeholder_text(
            GTK_ENTRY(entry), Tr("ui.forms.choice-placeholder").c_str());
        entry_row.entry = GTK_ENTRY(entry);
        control = entry;
        break;
      }
      case pdfium::FormFieldInfo::Kind::Text: {
        auto* entry = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(entry), field.value.c_str());
        entry_row.entry = GTK_ENTRY(entry);
        control = entry;
        break;
      }
    }
    auto const index = static_cast<gint>(rows_.size());
    g_object_set_data(G_OBJECT(control), "pdfgist-form-row",
                      GINT_TO_POINTER(index));
    gtk_box_append(GTK_BOX(card), control);
    rows_.push_back(std::move(entry_row));

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), card);
    gtk_list_box_append(list_, row);

    if (rows_.back().entry != nullptr) {
      hook(
          rows_.back().entry, "changed",
          +[](GtkEditable* editable, gpointer user_data) {
            auto* self = static_cast<FormsPane*>(user_data);
            if (self->populating_) return;  // rebuild echo, not user input
            self->apply_row(static_cast<std::size_t>(row_index(editable)));
          },
          this);
    }
    if (rows_.back().check != nullptr) {
      hook(
          rows_.back().check, "toggled",
          +[](GtkCheckButton* check, gpointer user_data) {
            auto* self = static_cast<FormsPane*>(user_data);
            if (self->populating_) return;
            self->apply_row(static_cast<std::size_t>(row_index(check)));
          },
          this);
    }
  }
  populating_ = false;
}

// One field value changed in the pane: push it into the in-memory document
// (values are never persisted — same as the mac host), then let the reader
// repaint the regenerated appearance streams.
void FormsPane::apply_row(std::size_t index) {
  if (index >= rows_.size()) return;
  FieldRow& row = rows_[index];
  auto* tab = ctx_.active_tab();
  if (tab == nullptr) return;
  bool ok = false;
  switch (row.field.kind) {
    case pdfium::FormFieldInfo::Kind::Check: {
      bool const on = gtk_check_button_get_active(row.check);
      row.desired = on ? "1" : "";
      ok = tab->document()->form_set_checked(row.field.page,
                                             row.field.annot_index, on);
      break;
    }
    case pdfium::FormFieldInfo::Kind::Choice: {
      std::string const value =
          gtk_editable_get_text(GTK_EDITABLE(row.entry));
      row.desired = value;
      ok = tab->document()->form_set_choice(row.field.page,
                                            row.field.annot_index, value);
      break;
    }
    case pdfium::FormFieldInfo::Kind::Text: {
      std::string const value =
          gtk_editable_get_text(GTK_EDITABLE(row.entry));
      row.desired = value;
      ok = tab->document()->form_set_text(row.field.page,
                                          row.field.annot_index, value);
      break;
    }
  }
  if (ok) ctx_.rendering_changed();
}

void FormsPane::export_filled() {
  auto* tab = ctx_.active_tab();
  if (tab == nullptr) return;
  if (tab->path().empty()) {
    set_status(Tr("ui.forms.export-disk-only"), true);
    return;
  }
  if (rows_.empty()) {
    set_status(Tr("ui.forms.export-none"), true);
    return;
  }
  std::string stem = tab->title();
  if (stem.size() >= 4 &&
      stem.compare(stem.size() - 4, 4, ".pdf") == 0) {
    stem.erase(stem.size() - 4);
  }
  auto* chooser = gtk_file_chooser_native_new(
      Tr("ui.forms.export").c_str(),
      GTK_WINDOW(gtk_widget_get_root(root_)), GTK_FILE_CHOOSER_ACTION_SAVE,
      Tr("ui.dialog.save").c_str(), Tr("ui.password.cancel").c_str());
  gtk_file_chooser_set_current_name(
      GTK_FILE_CHOOSER(chooser),
      (stem + Tr("ui.forms.export-suffix") + ".pdf").c_str());
  hook(
      chooser, "response",
      +[](GtkNativeDialog* native, int response, gpointer user_data) {
        auto* self = static_cast<FormsPane*>(user_data);
        if (response != GTK_RESPONSE_ACCEPT) {
          g_object_unref(native);
          return;
        }
        GFile* file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(native));
        g_object_unref(native);
        if (file == nullptr) return;
        gchar* path = g_file_get_path(file);
        g_object_unref(file);
        if (path == nullptr) return;
        std::string const target = path;
        g_free(path);

        auto* tab2 = self->ctx_.active_tab();
        if (tab2 == nullptr) return;
        std::string error;
        if (!tab2->document()->form_save(target, &error)) {
          self->set_status(Trf("ui.forms.export-failed", error), true);
          return;
        }
        // Honest count: re-read what actually committed (the FORM_* path
        // can fall back to the dictionary path, which non-merged fields may
        // reject).
        int filled = 0;
        std::string skipped;
        auto committed = tab2->document()->form_fields();
        for (auto const& row : self->rows_) {
          bool ok = false;
          for (auto const& field : committed) {
            if (field.name != row.field.name) continue;
            switch (row.field.kind) {
              case pdfium::FormFieldInfo::Kind::Check:
                ok = field.checked == (row.desired == "1");
                break;
              default:
                ok = field.value == row.desired;
                break;
            }
            break;
          }
          if (ok) {
            ++filled;
          } else {
            if (!skipped.empty()) skipped += ", ";
            skipped += row.field.name;
          }
        }
        if (skipped.empty()) {
          self->set_status(Trf("ui.forms.export-ok", std::to_string(filled)),
                           false);
        } else {
          self->set_status(
              Trf("ui.forms.export-skipped", std::to_string(filled),
                  std::to_string(self->rows_.size() - filled), skipped),
              true);
        }
      },
      this);
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
}

}  // namespace pdfgist::forms
