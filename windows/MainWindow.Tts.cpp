#include "pch.h"
#include "MainWindow.xaml.h"
#include "I18n.h"

#include <algorithm>
#include <cwctype>

namespace winrt::RivetHost::implementation {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace wam = winrt::Windows::Media;
namespace wamc = winrt::Windows::Media::Core;

// ---- TTS (read the current page aloud — mac toggleTTS semantics) --------------
//
// Windows.Media.SpeechSynthesis is initialized lazily on the first toggle so
// the 10s CI smoke never constructs the synthesizer or touches audio APIs.

void MainWindow::TtsButton_Click(winrt::Windows::Foundation::IInspectable const&,
                                 mux::RoutedEventArgs const&) {
  ToggleTts();
}

void MainWindow::ToggleTts() {
  if (tts_speaking_) {
    TtsStop();
    return;
  }
  auto tab = epub_active_ ? nullptr : active_pdf_.get();
  if (!tab || !tab->doc) return;
  int const page = tab->current_page;
  auto const doc = tab->doc;
  auto const dispatcher = DispatcherQueue();
  auto const weak = get_weak();
  std::thread([weak, dispatcher, doc, page]() mutable {
    std::string raw;
    try {
      raw = doc->PageText(page - 1);
    } catch (...) {
    }
    dispatcher.TryEnqueue([weak, raw = std::move(raw)]() mutable {
      if (auto window = weak.get()) {
        window->TtsSpeak(to_wide(raw));
      }
    });
  }).detach();
}

void MainWindow::TtsSpeak(std::wstring const& raw) {
  // trim like the mac host
  std::size_t begin = 0;
  std::size_t end = raw.size();
  while (begin < end && std::iswspace(raw[begin])) ++begin;
  while (end > begin && std::iswspace(raw[end - 1])) --end;
  std::wstring const text = raw.substr(begin, end - begin);
  if (text.empty()) {
    Alert(pdfgist::Tr("ui.tts.empty"));
    return;
  }
  try {
    if (tts_player_ == nullptr) {
      tts_synth_ = wam::SpeechSynthesis::SpeechSynthesizer();
      // Prefer a zh-CN voice so Chinese documents read naturally; fall back
      // to the system default otherwise.
      try {
        for (auto const& voice :
             wam::SpeechSynthesis::SpeechSynthesizer::AllVoices()) {
          if (voice.Language() == L"zh-CN") {
            tts_synth_.Voice(voice);
            break;
          }
        }
      } catch (...) {
      }
      tts_player_ = wam::Playback::MediaPlayer();
      auto const weak = get_weak();
      auto const dispatcher = DispatcherQueue();
      tts_player_.MediaEnded([weak, dispatcher](
                                 wam::Playback::MediaPlayer const&,
                                 winrt::Windows::Foundation::IInspectable const&) {
        dispatcher.TryEnqueue([weak]() {
          if (auto window = weak.get()) window->TtsSetSpeaking(false);
        });
      });
    }
    TtsSetSpeaking(true);
    // mac caps the utterance at 20k chars
    std::wstring const payload = text.substr(
        0, std::min<std::size_t>(text.size(), 20000));
    auto const weak = get_weak();
    auto const dispatcher = DispatcherQueue();
    auto operation =
        tts_synth_.SynthesizeTextToStreamAsync(payload);
    operation.Completed(
        [weak, dispatcher](
            winrt::Windows::Foundation::IAsyncOperation<
                wam::SpeechSynthesis::SpeechSynthesisStream> const& sender,
            winrt::Windows::Foundation::AsyncStatus status) {
          try {
            auto stream = sender.GetResults();
            dispatcher.TryEnqueue([weak, stream]() mutable {
              auto window = weak.get();
              if (!window) return;
              if (!window->tts_speaking_) return;  // stopped meanwhile
              try {
                auto source = wamc::MediaSource::CreateFromStream(
                    stream, stream.ContentType());
                window->tts_player_.Source(source);
                window->tts_player_.Play();
              } catch (...) {
                window->TtsSetSpeaking(false);
              }
            });
          } catch (...) {
            dispatcher.TryEnqueue([weak]() {
              if (auto window = weak.get()) window->TtsSetSpeaking(false);
            });
          }
          (void)status;
        });
  } catch (...) {
    TtsSetSpeaking(false);
    Alert(pdfgist::Tr("ui.tts.empty"));
  }
}

void MainWindow::TtsStop() {
  try {
    if (tts_player_ != nullptr) {
      tts_player_.Pause();
      tts_player_.Source(nullptr);
    }
    // Dropping the player source is the stop; a pending synthesis result is
    // discarded by the completed handler's speaking-state guard.
  } catch (...) {
  }
  TtsSetSpeaking(false);
}

void MainWindow::TtsSetSpeaking(bool speaking) {
  tts_speaking_ = speaking;
  TtsButton().IsChecked(speaking);
}

}  // namespace winrt::RivetHost::implementation
