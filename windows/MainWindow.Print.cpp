#include "pch.h"
#include "MainWindow.xaml.h"
#include "I18n.h"

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;

// ---- printing (whole document via the standard Windows print dialog) ----------
//
// PrintDlgEx owns the dialog; the GDI print loop renders each page with
// PDFium at the printer resolution and StretchDIBits it onto the page DC.
// Never runs at startup — the dialog only opens behind the print button.

void MainWindow::PrintButton_Click(
    winrt::Windows::Foundation::IInspectable const&,
    mux::RoutedEventArgs const&) {
  PrintDocument();
}

void MainWindow::PrintDocument() {
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab || !tab->doc) return;

  HWND hwnd = nullptr;
  if (auto const window_native = try_as<::IWindowNative>()) {
    (void)window_native->get_WindowHandle(&hwnd);
  }

  HGLOBAL dev_mode = nullptr;
  HGLOBAL dev_names = nullptr;
  HDC dc = nullptr;
  {
    PRINTDLGEX print{};
    print.lStructSize = sizeof(print);
    print.hwndOwner = hwnd;
    print.Flags = PD_RETURNDC | PD_USEDEVMODECOPIESANDCOLLATE |
                  PD_NOPAGENUMS | PD_NOSELECTION | PD_HIDEPRINTTOFILE;
    print.nCopies = 1;
    print.nStartPage = 0xFFFFFFFF;
    HRESULT const hr = ::PrintDlgEx(&print);
    if (FAILED(hr) || print.dwResultAction != PD_RESULT_PRINT ||
        print.hDC == nullptr) {
      if (print.hDevMode != nullptr) ::GlobalFree(print.hDevMode);
      if (print.hDevNames != nullptr) ::GlobalFree(print.hDevNames);
      if (print.hDC != nullptr) ::DeleteDC(print.hDC);
      return;  // user cancelled or no printer
    }
    dev_mode = print.hDevMode;
    dev_names = print.hDevNames;
    dc = print.hDC;
  }

  auto const doc = tab->doc;
  auto const title = tab->title;
  int const page_count = tab->page_count;

  // The render loop can take a while on long documents; keep it off the UI
  // thread (PDFium serializes on its own mutex).
  std::thread([doc, title, page_count, dc, dev_mode, dev_names]() mutable {
    bool ok = false;
    try {
      int const dpi_x = ::GetDeviceCaps(dc, LOGPIXELSX);
      int const dpi_y = ::GetDeviceCaps(dc, LOGPIXELSY);
      int const dpi = std::min(std::min(dpi_x, dpi_y), 300);
      if (dpi > 0) {
        // Cap raster at 300 dpi: letter-size pages stay ~33 MB, anything
        // higher only feeds the spooler bigger bitmaps.
        double const scale = dpi / 72.0;

        DOCINFOW doc_info{};
        doc_info.cbSize = sizeof(doc_info);
        doc_info.lpszDocName = title.c_str();
        ok = ::StartDocW(dc, &doc_info) > 0;

        for (int index = 0; ok && index < page_count; ++index) {
          double page_width = 612, page_height = 792;
          doc->PageSize(index, &page_width, &page_height);
          int const dest_width =
              std::max(1, static_cast<int>(std::lround(page_width * dpi / 72.0)));
          int const dest_height = std::max(
              1, static_cast<int>(std::lround(page_height * dpi / 72.0)));

          auto rendered = doc->RenderPage(index, scale, false);
          if (rendered.bgra.empty()) continue;

          BITMAPINFO info{};
          info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
          info.bmiHeader.biWidth = rendered.width;
          info.bmiHeader.biHeight = -rendered.height;  // top-down rows
          info.bmiHeader.biPlanes = 1;
          info.bmiHeader.biBitCount = 32;
          info.bmiHeader.biCompression = BI_RGB;

          if (::StartPage(dc) <= 0) continue;
          ::SetStretchBltMode(dc, COLORONCOLOR);
          ::StretchDIBits(dc, 0, 0, dest_width, dest_height, 0, 0,
                          rendered.width, rendered.height,
                          rendered.bgra.data(), &info, DIB_RGB_COLORS,
                          SRCCOPY);
          if (::EndPage(dc) < 0) continue;
        }
        ::EndDoc(dc);
      }
    } catch (...) {
      ok = false;
    }
    ::DeleteDC(dc);
    if (dev_mode != nullptr) ::GlobalFree(dev_mode);
    if (dev_names != nullptr) ::GlobalFree(dev_names);
    // Device-level failures were already surfaced by the print dialog; a
    // silent no-op matches the mac host's `op.run()` semantics.
    (void)ok;
  }).detach();
}

}  // namespace winrt::RivetHost::implementation
