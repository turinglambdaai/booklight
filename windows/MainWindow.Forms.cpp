#include "pch.h"
#include "MainWindow.xaml.h"
#include "GeneratedBackend.hpp"
#include "I18n.h"

#include <algorithm>
#include <thread>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace wu = winrt::Windows::UI;
namespace wut = winrt::Windows::UI::Text;

// ---- AcroForm pane (fill + export — the mac FormsPane reference) --------------
//
// Form values live in the open document's in-memory PDFium state only and are
// never persisted; export goes through FPDF_SaveAsCopy into a save dialog.
// Enumeration/fill run only when the pane is shown — the startup smoke never
// touches the form-fill environment.

namespace {

std::wstring form_stem(std::wstring const& title) {
  std::wstring const suffix = L".pdf";
  if (title.size() >= suffix.size() &&
      title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return title.substr(0, title.size() - suffix.size());
  }
  return title;
}

}  // namespace

void MainWindow::BuildFormsPane() {
  forms_pane_ = muxc::StackPanel();
  forms_pane_.Spacing(8);

  auto export_button = muxc::Button();
  muxc::TextBlock export_label;
  export_label.Text(pdfgist::Tr("ui.forms.export"));
  export_label.FontSize(12);
  export_label.Foreground(ThemeBrush({0xFF, 0xB4, 0x53, 0x09}));
  export_button.Content(export_label);
  export_button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
  export_button.Click(
      [weak = get_weak()](winrt::Windows::Foundation::IInspectable const&,
                          mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) window->ExportFilledPdf();
      });
  forms_pane_.Children().Append(export_button);

  forms_status_ = muxc::TextBlock();
  forms_status_.FontSize(11);
  forms_status_.Opacity(0.7);
  forms_status_.TextWrapping(mux::TextWrapping::Wrap);
  forms_status_.Visibility(mux::Visibility::Collapsed);
  forms_pane_.Children().Append(forms_status_);

  forms_list_ = muxc::StackPanel();
  forms_list_.Spacing(8);
  forms_pane_.Children().Append(forms_list_);
}

void MainWindow::ShowFormStatus(std::wstring const& text, bool is_error) {
  if (!forms_status_) return;
  if (text.empty()) {
    forms_status_.Visibility(mux::Visibility::Collapsed);
    return;
  }
  forms_status_.Text(text);
  forms_status_.Opacity(is_error ? 1.0 : 0.7);
  forms_status_.Foreground(ThemeBrush(is_error ? wu::Color{0xFF, 0xDC, 0x26, 0x26}
                                               : wu::Color{0xFF, 0x88, 0x88, 0x88}));
  forms_status_.Visibility(mux::Visibility::Visible);
}

void MainWindow::RefreshFormFields() {
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab || !tab->doc) {
    form_fields_.clear();
    if (forms_list_) forms_list_.Children().Clear();
    ShowFormStatus(pdfgist::Tr("ui.need.document"), true);
    return;
  }
  if (forms_busy_) return;
  forms_busy_ = true;
  ShowFormStatus(pdfgist::Tr("ui.forms.generating"), false);

  auto const doc = tab->doc;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, doc]() mutable {
    std::vector<FormField> fields;
    try {
      fields = doc->FormFields();
    } catch (...) {
    }
    dispatcher.TryEnqueue([weak, fields = std::move(fields)]() mutable {
      auto window = weak.get();
      if (!window) return;
      window->forms_busy_ = false;
      window->form_fields_ = std::move(fields);
      window->RebuildFormFieldsUi();
    });
  }).detach();
}

void MainWindow::RebuildFormFieldsUi() {
  if (!forms_list_) return;
  forms_list_.Children().Clear();
  form_text_values_.clear();
  form_check_values_.clear();

  if (form_fields_.empty()) {
    ShowFormStatus(pdfgist::Tr("ui.forms.empty"), false);
    return;
  }
  ShowFormStatus(
      pdfgist::Trf("ui.forms.count", std::to_wstring(form_fields_.size())),
      false);

  auto const weak = get_weak();
  for (auto const& field : form_fields_) {
    auto card = muxc::Border();
    card.Padding({10, 10, 10, 10});
    card.CornerRadius({8, 8, 8, 8});
    card.BorderThickness({1, 1, 1, 1});
    card.BorderBrush(ThemeBorderBrush());
    card.Background(ThemePanelBrush());

    auto stack = muxc::StackPanel();
    stack.Spacing(6);

    auto header = muxc::StackPanel();
    header.Orientation(muxc::Orientation::Horizontal);
    header.Spacing(6);
    auto kind_label = muxc::TextBlock();
    kind_label.FontSize(10);
    kind_label.FontWeight(wut::FontWeights::SemiBold());
    kind_label.Foreground(ThemeBrush({0xFF, 0xB4, 0x53, 0x09}));
    kind_label.Text(pdfgist::Tr(field.kind == 0   ? "ui.forms.type-text"
                                : field.kind == 1 ? "ui.forms.type-check"
                                                  : "ui.forms.type-choice"));
    kind_label.VerticalAlignment(mux::VerticalAlignment::Center);
    header.Children().Append(kind_label);
    auto name_label = muxc::TextBlock();
    name_label.Text(field.name);
    name_label.FontSize(12);
    name_label.TextTrimming(mux::TextTrimming::CharacterEllipsis);
    header.Children().Append(name_label);
    stack.Children().Append(header);

    std::wstring const name = field.name;
    if (field.kind == 1) {
      auto check = muxc::CheckBox();
      muxc::TextBlock check_label;
      check_label.Text(pdfgist::Tr("ui.forms.type-check"));
      check_label.FontSize(12);
      check.Content(check_label);
      check.IsChecked(field.checked);
      form_check_values_[name] = field.checked;
      check.Click([weak, name](
                      winrt::Windows::Foundation::IInspectable const&,
                      mux::RoutedEventArgs const&) {
        if (auto window = weak.get()) {
          bool const value = window->form_check_values_[name];
          window->form_check_values_[name] = !value;
          if (auto tab = window->active_pdf_) {
            auto const doc = tab->doc;
            bool const next = !value;
            std::thread([window_weak = weak, doc, name, next]() mutable {
              try {
                doc->SetFormCheck(name, next);
              } catch (...) {
              }
            }).detach();
          }
        }
      });
      stack.Children().Append(check);
    } else {
      auto input = muxc::TextBox();
      input.FontSize(12);
      input.Text(field.value);
      if (field.kind == 2) {
        input.PlaceholderText(pdfgist::Tr("ui.forms.choice-placeholder"));
      }
      form_text_values_[name] = field.value;
      // Apply on Enter / focus loss: the FORM_* fill path rewrites the field
      // and regenerates its appearance, too heavy per keystroke.
      input.KeyDown([weak, name](
                        winrt::Windows::Foundation::IInspectable const&,
                        mux::Input::KeyRoutedEventArgs const& args) {
        if (args.Key() != winrt::Windows::System::VirtualKey::Enter) return;
        if (auto window = weak.get()) window->ApplyFormFieldText(name);
        args.Handled(true);
      });
      input.LostFocus(
          [weak, name](winrt::Windows::Foundation::IInspectable const&,
                       mux::RoutedEventArgs const&) {
            if (auto window = weak.get()) window->ApplyFormFieldText(name);
          });
      input.TextChanged([weak, name, input](
                            winrt::Windows::Foundation::IInspectable const&,
                            muxc::TextChangedEventArgs const&) {
        if (auto window = weak.get()) {
          try {
            window->form_text_values_[name] = input.Text();
          } catch (...) {
          }
        }
      });
      stack.Children().Append(input);
    }

    card.Child(stack);
    forms_list_.Children().Append(card);
  }
}

void MainWindow::ApplyFormFieldText(std::wstring const& name) {
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab || !tab->doc) return;
  auto const found = form_text_values_.find(name);
  if (found == form_text_values_.end()) return;
  std::wstring value = found->second;
  auto const doc = tab->doc;
  std::thread([doc, name, value]() mutable {
    try {
      doc->SetFormFieldText(name, value);
    } catch (...) {
    }
  }).detach();
}

void MainWindow::ExportFilledPdf() {
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab || !tab->doc) return;
  if (forms_busy_) return;
  if (form_fields_.empty()) {
    ShowFormStatus(pdfgist::Tr("ui.forms.export-none"), true);
    return;
  }
  forms_busy_ = true;
  ShowFormStatus(pdfgist::Tr("ui.forms.generating"), false);

  int filled = 0;
  for (auto const& value : form_text_values_) {
    if (!value.second.empty()) ++filled;
  }
  for (auto const& value : form_check_values_) {
    if (value.second) ++filled;
  }

  auto const doc = tab->doc;
  auto const title = tab->title;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, doc, title, filled]() mutable {
    std::string failure;
    std::vector<std::uint8_t> data;
    try {
      data = doc->SaveToBytes();
      if (data.empty()) failure = "empty document data";
    } catch (std::exception const& e) {
      failure = e.what();
    }
    dispatcher.TryEnqueue(
        [weak, data = std::move(data), failure = std::move(failure),
         title = std::move(title), filled]() mutable {
          auto window = weak.get();
          if (!window) return;
          window->forms_busy_ = false;
          if (!failure.empty()) {
            window->ShowFormStatus(
                pdfgist::Trf("ui.forms.export-failed", winrt::to_hstring(failure)),
                true);
            return;
          }
          window->ShowFormStatus(
              pdfgist::Trf("ui.forms.export-ok", std::to_wstring(filled)), false);
          window->SaveEditResult(
              std::move(data),
              form_stem(title) + pdfgist::Tr("ui.forms.export-suffix"));
        });
  }).detach();
}

}  // namespace winrt::RivetHost::implementation
