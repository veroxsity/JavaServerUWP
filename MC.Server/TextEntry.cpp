#define _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.Core.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.ApplicationModel.Core.h>

#include <string>
#include <string_view>
#include <mutex>
#include <atomic>
#include <algorithm>

#include "Server.h"

namespace {

using namespace winrt::Windows::UI::Text::Core;

winrt::Windows::UI::Core::CoreWindow g_window{ nullptr };
CoreTextEditContext g_editContext{ nullptr };
winrt::Windows::UI::ViewManagement::InputPane g_inputPane{ nullptr };

winrt::event_token g_tTextRequested{};
winrt::event_token g_tSelectionRequested{};
winrt::event_token g_tTextUpdating{};
winrt::event_token g_tSelectionUpdating{};
winrt::event_token g_tLayoutRequested{};
winrt::event_token g_tFocusRemoved{};
winrt::event_token g_tKeyDown{};

std::mutex g_mutex;
std::wstring g_buffer;
int g_caret = 0;
std::atomic<bool> g_active{ false };
std::atomic<bool> g_submit{ false };

}

namespace textentry {

void Show(const std::wstring& initial, void* coreWindowAbi) {
    using namespace winrt::Windows::UI::Text::Core;
    if (g_active.load()) return;
    try {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_buffer = initial;
            g_caret = (int)g_buffer.size();
        }
        g_submit.store(false);

        winrt::Windows::UI::Core::CoreWindow w{ nullptr };
        winrt::copy_from_abi(w, coreWindowAbi);
        g_window = w;
        auto manager = CoreTextServicesManager::GetForCurrentView();
        g_editContext = manager.CreateEditContext();
        g_editContext.InputPaneDisplayPolicy(CoreTextInputPaneDisplayPolicy::Manual);
        g_editContext.InputScope(CoreTextInputScope::Text);

        g_tTextRequested = g_editContext.TextRequested(
            [](auto&&, CoreTextTextRequestedEventArgs const& args) {
                auto request = args.Request();
                auto range = request.Range();
                std::lock_guard<std::mutex> lk(g_mutex);
                const int len = (int)g_buffer.size();
                const int s = (std::max)(0, (std::min)(range.StartCaretPosition, len));
                const int e = (std::max)(s, (std::min)(range.EndCaretPosition, len));
                request.Text(winrt::hstring(std::wstring_view(g_buffer).substr(s, e - s)));
            });

        g_tSelectionRequested = g_editContext.SelectionRequested(
            [](auto&&, CoreTextSelectionRequestedEventArgs const& args) {
                args.Request().Selection(CoreTextRange{ g_caret, g_caret });
            });

        g_tTextUpdating = g_editContext.TextUpdating(
            [](auto&&, CoreTextTextUpdatingEventArgs const& args) {
                const auto range = args.Range();
                bool sawNewline = false;
                std::wstring incoming;
                for (wchar_t c : std::wstring_view(args.Text())) {
                    if (c == L'\r' || c == L'\n') { sawNewline = true; continue; }
                    if (c == L'\t') continue;
                    incoming.push_back(c);
                }
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    const int len = (int)g_buffer.size();
                    const int s = (std::max)(0, (std::min)(range.StartCaretPosition, len));
                    const int e = (std::max)(s, (std::min)(range.EndCaretPosition, len));
                    g_buffer = g_buffer.substr(0, s) + incoming + g_buffer.substr(e);
                    if (g_buffer.size() > 256) g_buffer.resize(256);
                    const int newLen = (int)g_buffer.size();
                    const auto sel = args.NewSelection();
                    g_caret = (std::max)(0, (std::min)(sel.EndCaretPosition, newLen));
                }
                if (sawNewline) g_submit.store(true);
                args.Result(CoreTextTextUpdatingResult::Succeeded);
            });

        g_tSelectionUpdating = g_editContext.SelectionUpdating(
            [](auto&&, CoreTextSelectionUpdatingEventArgs const& args) {
                g_caret = args.Selection().EndCaretPosition;
                args.Result(CoreTextSelectionUpdatingResult::Succeeded);
            });

        g_tLayoutRequested = g_editContext.LayoutRequested(
            [](auto&&, CoreTextLayoutRequestedEventArgs const& args) {
                winrt::Windows::Foundation::Rect rect{ 0.0f, 0.0f, 1.0f, 1.0f };
                if (g_window) rect = g_window.Bounds();
                auto bounds = args.Request().LayoutBounds();
                bounds.TextBounds(rect);
                bounds.ControlBounds(rect);
            });

        g_tFocusRemoved = g_editContext.FocusRemoved(
            [](auto&&, auto&&) { g_active.store(false); });

        g_tKeyDown = g_window.KeyDown(
            [](winrt::Windows::UI::Core::CoreWindow const&,
               winrt::Windows::UI::Core::KeyEventArgs const& args) {
                if (!g_active.load()) return;
                const auto vk = args.VirtualKey();
                if (vk == winrt::Windows::System::VirtualKey::Back) {
                    int newCaret = 0;
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lk(g_mutex);
                        int caret = (std::max)(0, (std::min)(g_caret, (int)g_buffer.size()));
                        if (caret > 0) {
                            g_buffer.erase(caret - 1, 1);
                            g_caret = caret - 1;
                            newCaret = g_caret;
                            changed = true;
                        }
                    }
                    if (changed && g_editContext) {
                        try {
                            g_editContext.NotifyTextChanged(CoreTextRange{ newCaret, newCaret + 1 }, 0, CoreTextRange{ newCaret, newCaret });
                            g_editContext.NotifySelectionChanged(CoreTextRange{ newCaret, newCaret });
                        } catch (...) {}
                    }
                    args.Handled(true);
                } else if (vk == winrt::Windows::System::VirtualKey::Enter ||
                           vk == winrt::Windows::System::VirtualKey::GamepadA) {
                    g_submit.store(true);
                    args.Handled(true);
                }
            });

        try { g_inputPane = winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView(); } catch (...) {}

        int len = 0;
        { std::lock_guard<std::mutex> lk(g_mutex); len = (int)g_buffer.size(); g_caret = len; }
        g_editContext.NotifyFocusEnter();
        g_editContext.NotifyTextChanged(CoreTextRange{ 0, 0 }, len, CoreTextRange{ len, len });
        g_editContext.NotifySelectionChanged(CoreTextRange{ len, len });
        if (g_inputPane) { try { g_inputPane.TryShow(); } catch (...) {} }

        g_active.store(true);
    } catch (...) {
        g_active.store(false);
        g_editContext = nullptr;
        g_inputPane = nullptr;
    }
}

void Hide() {
    if (g_editContext) {
        try { g_editContext.NotifyFocusLeave(); } catch (...) {}
        if (g_inputPane) { try { g_inputPane.TryHide(); } catch (...) {} }
        try {
            g_editContext.TextRequested(g_tTextRequested);
            g_editContext.SelectionRequested(g_tSelectionRequested);
            g_editContext.TextUpdating(g_tTextUpdating);
            g_editContext.SelectionUpdating(g_tSelectionUpdating);
            g_editContext.LayoutRequested(g_tLayoutRequested);
            g_editContext.FocusRemoved(g_tFocusRemoved);
        } catch (...) {}
    }
    if (g_window) { try { g_window.KeyDown(g_tKeyDown); } catch (...) {} }
    g_tKeyDown = {};
    g_editContext = nullptr;
    g_inputPane = nullptr;
    g_window = nullptr;
    g_active.store(false);
    g_submit.store(false);
}

bool Active() { return g_active.load(); }

std::wstring GetText() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_buffer;
}

bool ConsumeSubmit() { return g_submit.exchange(false); }

}

namespace appcontrol {

void RequestRestart() {
    try {
        winrt::Windows::ApplicationModel::Core::CoreApplication::RequestRestartAsync(winrt::hstring{});
    } catch (...) {}
}

}
