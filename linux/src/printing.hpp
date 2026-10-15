// Printing: GtkPrintOperation over the existing cairo/PDFium render path
// (mac printDocument semantics: scale-down-to-fit, auto page order). Runs
// only from the print action — never at startup.
#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string>

#include "pdfview.hpp"

namespace pdfgist::printing {

// Runs the print dialog for `doc`. Returns an empty string on success (the
// user cancelling included) or a human-readable failure reason.
std::string print_document(GtkWindow* parent,
                           std::shared_ptr<PdfDocument> const& doc,
                           int current_page  // 1-based
);

}  // namespace pdfgist::printing
