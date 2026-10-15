// Forms pane (AcroForm fill + export) — the v1 FORM tab, mac FormsPane.swift
// parity. Form filling is a view-layer concern on every host: values live in
// the open document only and are never persisted; export writes them out
// through FPDF_SaveAsCopy (mac uses PDFKit dataRepresentation).
#pragma once

#include <gtk/gtk.h>

#include <functional>
#include <string>
#include <vector>

#include "pdfview.hpp"

namespace pdfgist::forms {

struct PaneContext {
  std::function<PdfTab*()> active_tab;              // null without a PDF
  std::function<void(std::string const&)> alert;    // error surface
  std::function<void()> rendering_changed;          // appearance streams moved
};

class FormsPane {
 public:
  explicit FormsPane(PaneContext ctx);

  GtkWidget* widget() const { return root_; }

  // Re-enumerates the active document's fields (cheap: one PDFium pass).
  // Called on tab switches, page-edit reloads and first show.
  void refresh();

 private:
  struct FieldRow {
    pdfium::FormFieldInfo field;
    GtkEntry* entry{nullptr};        // text / choice
    GtkCheckButton* check{nullptr};  // check
    std::string desired;             // last value the user asked for
  };

  void set_status(std::string const& text, bool is_error);
  void apply_row(std::size_t index);
  void export_filled();

  PaneContext ctx_;
  GtkWidget* root_{nullptr};
  GtkListBox* list_{nullptr};
  GtkLabel* status_{nullptr};
  GtkButton* export_{nullptr};
  std::vector<FieldRow> rows_;
  bool populating_{false};
};

}  // namespace pdfgist::forms
