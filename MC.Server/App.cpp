#include "Server.h"

class ConsoleRingBuffer {
public:
    static const size_t MAX_LINES = RuntimeConfig::CONSOLE_BUFFER_LINES;

    struct Line {
        std::wstring text;
    };

    ConsoleRingBuffer() : m_lines(MAX_LINES), m_writeIdx(0), m_count(0) {}

    void Push(const std::wstring& text) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines[m_writeIdx].text = text;
        m_writeIdx = (m_writeIdx + 1) % MAX_LINES;
        if (m_count < MAX_LINES) m_count++;
    }

    size_t Count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }

    const Line& At(size_t index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t start = m_count < MAX_LINES ? 0 : m_writeIdx;
        size_t actual = (start + index) % MAX_LINES;
        return m_lines[actual];
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_writeIdx = 0;
        m_count = 0;
    }

private:
    std::vector<Line> m_lines;
    size_t m_writeIdx;
    size_t m_count;
    mutable std::mutex m_mutex;
};

class LogFileReader {
public:
    LogFileReader(const std::wstring& path) : m_path(path), m_lastSize(0) {}

    std::vector<std::wstring> PollNewLines() {
        std::vector<std::wstring> lines;
        std::ifstream file(m_path, std::ios::binary);
        if (!file.is_open()) return lines;

        file.seekg(0, std::ios::end);
        size_t currentSize = (size_t)file.tellg();
        if (currentSize <= m_lastSize) return lines;

        file.seekg((std::streamoff)m_lastSize, std::ios::beg);
        std::vector<char> buf(currentSize - m_lastSize);
        file.read(buf.data(), buf.size());
        m_lastSize = currentSize;

        std::wstring currentLine;
        std::string raw(buf.data(), buf.size());
        for (char ch : raw) {
            if (ch == '\n' || ch == '\r') {
                if (ch == '\r') continue;
                if (!currentLine.empty()) {
                    lines.push_back(currentLine);
                    currentLine.clear();
                }
            } else {
                currentLine += Utf8ToWide(std::string(1, ch));
            }
        }
        if (!currentLine.empty()) {
            lines.push_back(currentLine);
        }

        return lines;
    }

    void Reset() {
        m_lastSize = 0;
    }

private:
    std::wstring m_path;
    size_t m_lastSize;
};

class ConsoleRenderer {
public:
    ConsoleRenderer() : m_dpi(96.0f), m_charHeight(0.0f), m_charWidth(0.0f),
        m_visibleLines(0), m_visibleCols(0), m_scrollOffset(0) {}

    bool Initialize(ComPtr<ID2D1DeviceContext> d2dContext) {
        m_d2dContext = d2dContext;
        FLOAT dpiX = 96.0f, dpiY = 96.0f;
        m_dpi = dpiX;

        HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
            __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf()));
        if (FAILED(hr)) return false;

        hr = m_dwriteFactory->CreateTextFormat(
            L"Consolas", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            RuntimeConfig::CONSOLE_FONT_SIZE,
            L"en-us",
            m_textFormat.GetAddressOf());
        if (FAILED(hr)) {
            hr = m_dwriteFactory->CreateTextFormat(
                L"Courier New", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                RuntimeConfig::CONSOLE_FONT_SIZE,
                L"en-us",
                m_textFormat.GetAddressOf());
        }
        if (FAILED(hr)) return false;

        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        hr = d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(D2D1::ColorF::LightGray), m_textBrush.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.9f), m_bgBrush.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.2f, 0.6f, 0.2f), m_inputBrush.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.8f, 0.8f, 0.2f), m_statusBrush.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.93f, 0.35f, 0.32f), m_errorBrush.GetAddressOf());
        if (FAILED(hr)) return false;

        hr = d2dContext->CreateSolidColorBrush(
            D2D1::ColorF(0.10f, 0.12f, 0.20f), m_headerBgBrush.GetAddressOf());
        if (FAILED(hr)) return false;

        DWRITE_TEXT_METRICS metrics;
        ComPtr<IDWriteTextLayout> measureLayout;
        m_dwriteFactory->CreateTextLayout(
            L"M", 1, m_textFormat.Get(),
            1000.0f, 1000.0f,
            measureLayout.GetAddressOf());
        float measuredWidth = 0;
        if (measureLayout) {
            measureLayout->GetMetrics(&metrics);
            m_charHeight = metrics.height;
            DWRITE_OVERHANG_METRICS overhang;
            measureLayout->GetOverhangMetrics(&overhang);
            measuredWidth = metrics.widthIncludingTrailingWhitespace;
        }
        m_charWidth = measuredWidth > 0 ? measuredWidth : (RuntimeConfig::CONSOLE_FONT_SIZE * 0.6f);

        m_scaleFactor = 1.0f;
        m_d2dContext->GetDpi(&dpiX, &dpiY);

        return true;
    }

    void SetBounds(float width, float height, float scale) {
        m_width = width / scale;
        m_height = height / scale;
        m_visibleLines = (int)(m_height / m_charHeight) - 1;
        m_visibleCols = (int)(m_width / m_charWidth);
        m_scaleFactor = scale;

        if (m_visibleLines < 1) m_visibleLines = 1;
        if (m_visibleCols < 1) m_visibleCols = 40;
    }

    ID2D1SolidColorBrush* BrushForLine(const std::wstring& s) {
        if (s.rfind(L"[ERR]", 0) == 0 ||
            s.find(L"/ERROR]") != std::wstring::npos ||
            s.find(L"/FATAL]") != std::wstring::npos)
            return m_errorBrush.Get();
        if (s.find(L"/WARN]") != std::wstring::npos)
            return m_statusBrush.Get();
        return m_textBrush.Get();
    }

    void Render(ConsoleRingBuffer& buffer, const std::wstring& inputLine,
                const std::wstring& statusText, const std::wstring& headerLabel, bool running) {
        if (!m_d2dContext) return;

        auto ctx = m_d2dContext;
        ctx->BeginDraw();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scaleFactor, m_scaleFactor));
        ctx->Clear(D2D1::ColorF(0.05f, 0.05f, 0.1f));

        D2D1_RECT_F bgRect = D2D1::RectF(0, 0, m_width, m_height);
        ctx->FillRectangle(bgRect, m_bgBrush.Get());

        float headerH = m_charHeight * 1.7f;
        ctx->FillRectangle(D2D1::RectF(0, 0, m_width, headerH), m_headerBgBrush.Get());
        float hdrTextY = (headerH - m_charHeight) * 0.5f;
        std::wstring left = (headerLabel.empty() ? std::wstring(L"JavaServerUWP") : headerLabel)
                            + L"   |   " + std::to_wstring(buffer.Count()) + L" lines";
        DrawLine(ctx, left, m_charWidth, hdrTextY, m_textBrush.Get());
        std::wstring state = running ? L"RUNNING" : L"STOPPED";
        std::wstring hint = running ? L"A keyboard" : L"A restart";
        float stateX = m_width - m_charWidth * (float)(state.size() + 2);
        float hintX = stateX - m_charWidth * (float)(hint.size() + 3);
        DrawLine(ctx, hint, hintX, hdrTextY, m_statusBrush.Get());
        DrawLine(ctx, state, stateX, hdrTextY, running ? m_inputBrush.Get() : m_errorBrush.Get());

        float logBottom = m_height - m_charHeight * 1.6f;
        int logLines = (int)((logBottom - headerH) / m_charHeight);
        if (logLines < 1) logLines = 1;

        size_t totalLines = buffer.Count();
        size_t startIdx = 0;
        if (totalLines > (size_t)logLines) {
            startIdx = totalLines - logLines;
        }
        int scrollAdj = m_scrollOffset;
        if (scrollAdj > 0 && (size_t)scrollAdj > startIdx) {
            startIdx = 0;
        } else if (scrollAdj > 0) {
            startIdx -= scrollAdj;
        } else if (scrollAdj < 0) {
            startIdx = std::min(totalLines, startIdx + (size_t)(-scrollAdj));
        }

        for (int i = 0; i < logLines && (startIdx + i) < totalLines; ++i) {
            const auto& line = buffer.At(startIdx + i);
            RenderTextLine(ctx, line.text, headerH + (float)i * m_charHeight, BrushForLine(line.text));
        }

        float inputY = m_height - m_charHeight * 1.2f;
        D2D1_RECT_F sepRect = D2D1::RectF(0, inputY - 2, m_width, inputY - 1);
        ctx->FillRectangle(sepRect, m_textBrush.Get());

        std::wstring promptText = L"> " + inputLine;
        if (m_inputCursorVisible) {
            promptText += L"_";
        }
        RenderTextLine(ctx, promptText, inputY, m_inputBrush.Get());

        if (m_scrollOffset != 0) {
            std::wstring scrollMsg = L"[scrolled] ";
            DWRITE_TEXT_METRICS metrics;
            ComPtr<IDWriteTextLayout> indicatorLayout;
            m_dwriteFactory->CreateTextLayout(
                scrollMsg.c_str(), (UINT32)scrollMsg.size(),
                m_textFormat.Get(), m_width, m_charHeight,
                indicatorLayout.GetAddressOf());
            if (indicatorLayout) {
                indicatorLayout->GetMetrics(&metrics);
                ctx->DrawTextLayout(
                    D2D1::Point2F(m_width - metrics.width - 8.0f, 4.0f),
                    indicatorLayout.Get(), m_statusBrush.Get());
            }
        }

        ctx->EndDraw();
    }

    void RenderLauncher(const std::vector<std::wstring>& items, int selected,
                        const std::wstring& message, const std::wstring& webLine) {
        if (!m_d2dContext) return;
        auto ctx = m_d2dContext;
        ctx->BeginDraw();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scaleFactor, m_scaleFactor));
        ctx->Clear(D2D1::ColorF(0.05f, 0.05f, 0.1f));

        float x = m_charWidth * 4.0f;
        float y = m_charHeight * 2.0f;
        DrawLine(ctx, L"JavaServerUWP", x, y, m_statusBrush.Get());
        y += m_charHeight * 1.8f;
        DrawLine(ctx, L"Your servers", x, y, m_textBrush.Get());
        y += m_charHeight * 2.2f;

        if (items.empty()) {
            DrawLine(ctx, L"No servers yet.", x, y, m_textBrush.Get());
            y += m_charHeight * 1.3f;
            DrawLine(ctx, L"Press Y to create one.", x, y, m_textBrush.Get());
        } else {
            for (int i = 0; i < (int)items.size(); ++i) {
                bool sel = (i == selected);
                std::wstring row = (sel ? L"> " : L"  ") + items[i];
                DrawLine(ctx, row, x, y, sel ? m_inputBrush.Get() : m_textBrush.Get());
                y += m_charHeight * 1.3f;
            }
        }

        float footY = m_height - m_charHeight * 1.5f;
        if (!webLine.empty()) {
            DrawLine(ctx, webLine, x, footY - m_charHeight * 2.8f, m_inputBrush.Get());
        }
        if (!message.empty()) {
            DrawLine(ctx, message, x, footY - m_charHeight * 1.4f, m_statusBrush.Get());
        }
        DrawLine(ctx, L"A start   Up/Down select   Y new server   X delete", x, footY, m_statusBrush.Get());

        ctx->EndDraw();
    }

    void RenderNewServer(const std::vector<std::wstring>& items, int selected,
                         const std::wstring& message) {
        if (!m_d2dContext) return;
        auto ctx = m_d2dContext;
        ctx->BeginDraw();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scaleFactor, m_scaleFactor));
        ctx->Clear(D2D1::ColorF(0.05f, 0.05f, 0.1f));

        float x = m_charWidth * 4.0f;
        float y = m_charHeight * 2.0f;
        DrawLine(ctx, L"New server", x, y, m_statusBrush.Get());
        y += m_charHeight * 1.8f;
        DrawLine(ctx, L"Pick a version, then press A to name and create it", x, y, m_textBrush.Get());
        y += m_charHeight * 2.2f;

        if (items.empty()) {
            DrawLine(ctx, L"No versions available.", x, y, m_textBrush.Get());
            y += m_charHeight * 1.3f;
            DrawLine(ctx, L"Press Y to download one.", x, y, m_textBrush.Get());
        } else {
            for (int i = 0; i < (int)items.size(); ++i) {
                bool sel = (i == selected);
                std::wstring row = (sel ? L"> " : L"  ") + items[i];
                DrawLine(ctx, row, x, y, sel ? m_inputBrush.Get() : m_textBrush.Get());
                y += m_charHeight * 1.3f;
            }
        }

        float footY = m_height - m_charHeight * 1.5f;
        if (!message.empty()) {
            DrawLine(ctx, message, x, footY - m_charHeight * 1.4f, m_statusBrush.Get());
        }
        DrawLine(ctx, L"A create   Up/Down select   Y download versions   X remove   B back", x, footY, m_statusBrush.Get());

        ctx->EndDraw();
    }

    void RenderNaming(const std::wstring& typed, const std::wstring& sourceLabel) {
        if (!m_d2dContext) return;
        auto ctx = m_d2dContext;
        ctx->BeginDraw();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scaleFactor, m_scaleFactor));
        ctx->Clear(D2D1::ColorF(0.05f, 0.05f, 0.1f));

        float x = m_charWidth * 4.0f;
        float y = m_charHeight * 2.0f;
        DrawLine(ctx, L"Name your server", x, y, m_statusBrush.Get());
        y += m_charHeight * 1.8f;
        if (!sourceLabel.empty()) {
            DrawLine(ctx, L"From " + sourceLabel, x, y, m_textBrush.Get());
            y += m_charHeight * 2.0f;
        } else {
            y += m_charHeight * 0.7f;
        }

        DrawLine(ctx, L"Server name:", x, y, m_textBrush.Get());
        y += m_charHeight * 1.4f;
        std::wstring line = typed + (m_inputCursorVisible ? L"_" : L" ");
        DrawLine(ctx, line, x, y, m_inputBrush.Get());

        float footY = m_height - m_charHeight * 1.5f;
        DrawLine(ctx, L"Type a name on the keyboard, then Enter to create", x, footY, m_statusBrush.Get());

        ctx->EndDraw();
    }

    void RenderDownloads(const std::vector<std::wstring>& items, int selected,
                         const std::wstring& status, int progress) {
        if (!m_d2dContext) return;
        auto ctx = m_d2dContext;
        ctx->BeginDraw();
        ctx->SetTransform(D2D1::Matrix3x2F::Scale(m_scaleFactor, m_scaleFactor));
        ctx->Clear(D2D1::ColorF(0.05f, 0.05f, 0.1f));

        float x = m_charWidth * 4.0f;
        float y = m_charHeight * 2.0f;
        DrawLine(ctx, L"Download a server version (Paper)", x, y, m_statusBrush.Get());
        y += m_charHeight * 1.8f;

        std::wstring sub = status;
        if (progress >= 0) sub += L"  " + std::to_wstring(progress) + L"%";
        if (!sub.empty()) {
            DrawLine(ctx, sub, x, y, m_textBrush.Get());
        }
        y += m_charHeight * 2.0f;

        float listBottom = m_height - m_charHeight * 2.5f;
        int rows = (int)((listBottom - y) / (m_charHeight * 1.2f));
        if (rows < 1) rows = 1;
        int total = (int)items.size();
        int start = 0;
        if (total > rows) {
            start = selected - rows / 2;
            if (start < 0) start = 0;
            if (start > total - rows) start = total - rows;
        }
        for (int i = start; i < total && i < start + rows; ++i) {
            bool sel = (i == selected);
            std::wstring row = (sel ? L"> " : L"  ") + items[i];
            DrawLine(ctx, row, x, y, sel ? m_inputBrush.Get() : m_textBrush.Get());
            y += m_charHeight * 1.2f;
        }
        if (total > rows) {
            std::wstring pos = std::to_wstring(selected + 1) + L" / " + std::to_wstring(total);
            DrawLine(ctx, pos, x, listBottom, m_textBrush.Get());
        }

        float footY = m_height - m_charHeight * 1.5f;
        const wchar_t* foot = items.empty()
            ? L"A retry    B back"
            : L"A download    Up / Down select    Y refresh    B back";
        DrawLine(ctx, foot, x, footY, m_statusBrush.Get());

        ctx->EndDraw();
    }

    void ScrollUp(int lines) { m_scrollOffset += lines; }
    void ScrollDown(int lines) { m_scrollOffset -= lines; }
    void ResetScroll() { m_scrollOffset = 0; }

    void ToggleCursor() { m_inputCursorVisible = !m_inputCursorVisible; }

    int VisibleLines() const { return m_visibleLines; }

private:
    void RenderTextLine(ComPtr<ID2D1DeviceContext> ctx, const std::wstring& text,
                        float y, ID2D1SolidColorBrush* brush) {
        if (text.empty()) return;

        std::wstring display = text;
        if ((int)display.size() > m_visibleCols) {
            display = display.substr(display.size() - m_visibleCols);
        }

        ComPtr<IDWriteTextLayout> layout;
        m_dwriteFactory->CreateTextLayout(
            display.c_str(), (UINT32)display.size(),
            m_textFormat.Get(), m_width, m_charHeight,
            layout.GetAddressOf());
        if (layout) {
            ctx->DrawTextLayout(D2D1::Point2F(0, y), layout.Get(), brush);
        }
    }

    void DrawLine(ComPtr<ID2D1DeviceContext> ctx, const std::wstring& text,
                  float x, float y, ID2D1SolidColorBrush* brush) {
        if (text.empty()) return;
        ComPtr<IDWriteTextLayout> layout;
        m_dwriteFactory->CreateTextLayout(text.c_str(), (UINT32)text.size(),
            m_textFormat.Get(), m_width, m_charHeight, layout.GetAddressOf());
        if (layout) ctx->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush);
    }

    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<IDWriteFactory> m_dwriteFactory;
    ComPtr<IDWriteTextFormat> m_textFormat;
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    ComPtr<ID2D1SolidColorBrush> m_inputBrush;
    ComPtr<ID2D1SolidColorBrush> m_statusBrush;
    ComPtr<ID2D1SolidColorBrush> m_errorBrush;
    ComPtr<ID2D1SolidColorBrush> m_headerBgBrush;
    float m_dpi;
    float m_charHeight;
    float m_charWidth;
    float m_width;
    float m_height;
    int m_visibleLines;
    int m_visibleCols;
    int m_scrollOffset;
    float m_scaleFactor;
    bool m_inputCursorVisible = true;
};

class App : public IFrameworkView
{
public:
    App() : m_refCount(1), m_windowClosed(false), m_windowVisible(true),
        m_serverStarted(false), m_serverSuccess(false) {}

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IFrameworkView) || riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable)) {
            *ppv = static_cast<IFrameworkView*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    STDMETHODIMP GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
    STDMETHODIMP GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }

    STDMETHODIMP Initialize(_In_ ICoreApplicationView*) override {
        WriteLaunchLog(L"App::Initialize");
        return S_OK;
    }

    STDMETHODIMP SetWindow(_In_ ICoreWindow* window) override {
        WriteLaunchLog(L"App::SetWindow: entered");
        m_coreWindow = window;

        ComPtr<ABI::Windows::UI::Core::ICoreWindow> wnd(window);

        auto keyHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::ITypedEventHandler<
                ABI::Windows::UI::Core::CoreWindow*,
                ABI::Windows::UI::Core::KeyEventArgs*>
        >([this](ABI::Windows::UI::Core::ICoreWindow* sender,
                 ABI::Windows::UI::Core::IKeyEventArgs* args) -> HRESULT {
            OnKeyDown(sender, args);
            return S_OK;
        });

        ComPtr<ABI::Windows::UI::Core::ICoreWindow> coreWnd(window);
        EventRegistrationToken keyToken;
        coreWnd->add_KeyDown(keyHandler.Get(), &keyToken);
        {
            ComPtr<ABI::Windows::UI::Core::ISystemNavigationManagerStatics> navStatics;
            if (SUCCEEDED(GetActivationFactory(
                    HStringReference(RuntimeClass_Windows_UI_Core_SystemNavigationManager).Get(),
                    navStatics.GetAddressOf()))) {
                ComPtr<ABI::Windows::UI::Core::ISystemNavigationManager> navManager;
                if (SUCCEEDED(navStatics->GetForCurrentView(navManager.GetAddressOf()))) {
                    EventRegistrationToken backToken;
                    navManager->add_BackRequested(
                        Microsoft::WRL::Callback<ABI::Windows::Foundation::IEventHandler<
                            ABI::Windows::UI::Core::BackRequestedEventArgs*>>(
                            [](IInspectable*, ABI::Windows::UI::Core::IBackRequestedEventArgs* args) -> HRESULT {
                                if (args) args->put_Handled(TRUE);
                                return S_OK;
                            }).Get(),
                        &backToken);
                    WriteLaunchLog(L"SetWindow: BackRequested handler installed");
                }
            }
        }

        auto visHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::ITypedEventHandler<
                ABI::Windows::UI::Core::CoreWindow*,
                ABI::Windows::UI::Core::VisibilityChangedEventArgs*>
        >([this](ABI::Windows::UI::Core::ICoreWindow* sender,
                 ABI::Windows::UI::Core::IVisibilityChangedEventArgs* e) -> HRESULT {
            boolean visible;
            e->get_Visible(&visible);
            m_windowVisible = visible;
            return S_OK;
        });

        coreWnd->add_VisibilityChanged(visHandler.Get(), &m_visToken);

        auto closeHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::ITypedEventHandler<
                ABI::Windows::UI::Core::CoreWindow*,
                ABI::Windows::UI::Core::CoreWindowEventArgs*>
        >([this](ABI::Windows::UI::Core::ICoreWindow*,
                 ABI::Windows::UI::Core::ICoreWindowEventArgs*) -> HRESULT {
            m_windowClosed = true;
            return S_OK;
        });

        coreWnd->add_Closed(closeHandler.Get(), &m_closeToken);
        coreWnd->Activate();
        WriteLaunchLog(L"App::SetWindow: window activated");

        return S_OK;
    }

    STDMETHODIMP Load(_In_ HSTRING) override {
        WriteLaunchLog(L"App::Load");
        return S_OK;
    }

    STDMETHODIMP Run() override {
        WriteLaunchLog(L"App::Run: entered");

        if (!InitD2D()) {
            WriteLaunchLog(L"App::Run: InitD2D FAILED, continuing without rendering");
        } else {
            WriteLaunchLog(L"App::Run: InitD2D OK");
        }

        RequestForegroundExtendedExecution();

        std::wstring localState = ResolveLocalStateDir();
        std::wstring exeDir = GetModuleDir();

        std::wstring stdoutLog = localState + L"\\" + std::wstring(RuntimeConfig::STDOUT_LOG_FILE);
        std::wstring stderrLog = localState + L"\\" + std::wstring(RuntimeConfig::STDERR_LOG_FILE);
        for (const std::wstring& p : { stdoutLog, stderrLog }) {
            HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }

        LogFileReader stdoutReader(stdoutLog);
        LogFileReader stderrReader(stderrLog);

        m_ctx.localStateDir = localState;
        m_ctx.exeDir = exeDir;
        m_ctx.stdoutLogPath = stdoutLog;
        m_ctx.stderrLogPath = stderrLog;
        m_ctx.shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_ctx.pSuccess = &m_serverSuccess;

        EnsureDirRecursive(localState + L"\\instances");
        m_versions = DiscoverServerVersions(localState, exeDir);
        m_versionSel = 0;
        m_instances = DiscoverInstances(localState);
        m_instanceSel = 0;
        m_screen = Screen::Launcher;
        WriteLaunchLogF(L"App::Run: %zu instances, %zu pool versions, showing launcher",
                        m_instances.size(), m_versions.size());

        webpanel::SetStatusProvider([]() { return (bool)g_serverRunning; });
        webpanel::Start(localState + L"\\instances");
        m_webLine = L"Web file panel:  " + webpanel::Url();
        std::wstring statusText = L"Server starting...";
        std::vector<std::wstring> pendingLines;

        LARGE_INTEGER perfFreq;
        QueryPerformanceFrequency(&perfFreq);
        LARGE_INTEGER lastCursorToggle;
        QueryPerformanceCounter(&lastCursorToggle);
        const double cursorInterval = 0.5;

        ComPtr<ABI::Windows::UI::Core::ICoreWindow> coreWnd(m_coreWindow);
        ComPtr<ABI::Windows::UI::Core::ICoreDispatcher> dispatcher;
        coreWnd->get_Dispatcher(&dispatcher);

        while (!m_windowClosed && !g_shutdownRequested) {
            dispatcher->ProcessEvents(
                ABI::Windows::UI::Core::CoreProcessEventsOption_ProcessAllIfPresent);

            if (m_versionsDirty.exchange(false)) {
                m_versions = DiscoverServerVersions(m_ctx.localStateDir, m_ctx.exeDir);
                if (m_versionSel >= (int)m_versions.size())
                    m_versionSel = m_versions.empty() ? 0 : (int)m_versions.size() - 1;
            }

            if (m_screen == Screen::Console) {
                auto stdoutLines = stdoutReader.PollNewLines();
                auto stderrLines = stderrReader.PollNewLines();

                for (auto& line : stdoutLines) {
                    if (IsConsoleNoise(line)) continue;
                    m_consoleBuffer.Push(line);
                }
                for (auto& line : stderrLines) {
                    if (IsConsoleNoise(line)) continue;
                    m_consoleBuffer.Push(L"[ERR] " + line);
                }

                if (textentry::ConsumeSubmit()) {
                    std::wstring t = textentry::GetText();
                    textentry::Hide();
                    WriteToStdin(WideToUtf8(t) + "\n");
                    m_consoleBuffer.Push(L"> " + t);
                    m_commandInput.clear();
                    m_consoleRenderer.ResetScroll();
                    m_keyboardWasActive = false;
                } else if (textentry::Active()) {
                    m_commandInput = textentry::GetText();
                    m_keyboardWasActive = true;
                } else if (m_keyboardWasActive) {
                    m_commandInput = textentry::GetText();
                    textentry::Hide();
                    m_keyboardWasActive = false;
                }

                if (g_serverRunning) {
                    statusText = L"RUNNING | " + Utf8ToWide(std::to_string(m_consoleBuffer.Count())) + L" lines   |   A keyboard";
                } else if (m_serverStarted) {
                    statusText = m_serverSuccess ? L"Server stopped" : L"Server exited with errors";
                }
            }

            if (m_screen == Screen::NewServer && m_namingInstance) {
                if (textentry::ConsumeSubmit()) {
                    std::wstring name = textentry::GetText();
                    textentry::Hide();
                    m_namingInstance = false;
                    m_nameKbWasActive = false;
                    m_instanceNameInput.clear();
                    FinishCreateInstance(name);
                } else if (textentry::Active()) {
                    m_instanceNameInput = textentry::GetText();
                    m_nameKbWasActive = true;
                } else if (m_nameKbWasActive) {
                    textentry::Hide();
                    m_namingInstance = false;
                    m_nameKbWasActive = false;
                    m_instanceNameInput.clear();
                }
            }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            double elapsed = (double)(now.QuadPart - lastCursorToggle.QuadPart) /
                             (double)perfFreq.QuadPart;
            if (elapsed >= cursorInterval) {
                m_consoleRenderer.ToggleCursor();
                lastCursorToggle = now;
            }

            if (m_d2dContext && m_consoleRendererInitialized && m_windowVisible) {
                if (m_screen == Screen::Launcher) {
                    std::vector<std::wstring> items;
                    for (auto& inst : m_instances) items.push_back(inst.label);
                    m_consoleRenderer.RenderLauncher(items, m_instanceSel, m_launcherMsg, m_webLine);
                } else if (m_screen == Screen::NewServer) {
                    if (m_namingInstance) {
                        std::wstring src = (m_createFromIdx >= 0 && m_createFromIdx < (int)m_versions.size())
                                               ? m_versions[m_createFromIdx].label : std::wstring();
                        m_consoleRenderer.RenderNaming(m_instanceNameInput, src);
                    } else {
                        std::vector<std::wstring> items;
                        for (auto& v : m_versions) items.push_back(v.label);
                        m_consoleRenderer.RenderNewServer(items, m_versionSel, m_launcherMsg);
                    }
                } else if (m_screen == Screen::Downloads) {
                    std::vector<std::wstring> items; std::wstring st; int sel;
                    {
                        std::lock_guard<std::mutex> lk(m_dlMutex);
                        items = m_dlVersions; st = m_dlStatus; sel = m_dlSel;
                    }
                    m_consoleRenderer.RenderDownloads(items, sel, st, m_dlProgress.load());
                } else {
                    m_consoleRenderer.Render(m_consoleBuffer, m_commandInput, statusText,
                                             m_runningLabel, g_serverRunning);
                }
                if (m_swapChain) {
                    m_swapChain->Present(1, 0);
                }
            }
            {
                static ULONGLONG lastHb = 0;
                ULONGLONG nowTick = GetTickCount64();
                if (nowTick - lastHb >= 1000) {
                    static int hbCount = 0;
                    WriteLaunchLogF(L"main: heartbeat #%d windowVisible=%d windowClosed=%d serverRunning=%d",
                        ++hbCount, m_windowVisible ? 1 : 0, m_windowClosed ? 1 : 0, g_serverRunning ? 1 : 0);
                    lastHb = nowTick;
                }
            }

            Sleep(16);
        }

        if (g_serverRunning) {
            OutputDebugStringW(L"[MC.Server] Sending 'stop' to server...");
            WriteToStdin("stop\n");
            WaitForSingleObject(m_ctx.shutdownEvent, 30000);
        }

        if (m_serverThread) {
            WaitForSingleObject(m_serverThread, 5000);
            CloseHandle(m_serverThread);
            m_serverThread = nullptr;
        }

        CloseHandle(m_ctx.shutdownEvent);
        return S_OK;
    }

    STDMETHODIMP Uninitialize() override {
        m_consoleRendererInitialized = false;
        m_textFormat.Reset();
        m_textBrush.Reset();
        m_bgBrush.Reset();
        m_inputBrush.Reset();
        m_statusBrush.Reset();
        m_d2dContext.Reset();
        m_swapChain.Reset();
        m_d3dDevice.Reset();
        m_dxgiDevice.Reset();
        m_d2dDevice.Reset();
        return S_OK;
    }

private:
    bool InitD2D() {
        WriteLaunchLog(L"InitD2D: begin");

        UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, creationFlags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
            &d3dDevice, nullptr, &d3dContext);
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: hardware D3D11CreateDevice failed hr=0x%08X, trying WARP", hr);
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, creationFlags,
                featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
                &d3dDevice, nullptr, &d3dContext);
        }
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: D3D11CreateDevice (WARP) failed hr=0x%08X", hr);
            return false;
        }
        WriteLaunchLog(L"InitD2D: D3D11 device created");

        m_d3dDevice = d3dDevice;

        hr = d3dDevice.As(&m_dxgiDevice);
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: As(IDXGIDevice1) failed hr=0x%08X", hr);
            return false;
        }

        ComPtr<ID2D1Factory1> d2dFactory;
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1), nullptr, (void**)&d2dFactory);
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: D2D1CreateFactory failed hr=0x%08X", hr);
            return false;
        }

        hr = d2dFactory->CreateDevice(m_dxgiDevice.Get(), &m_d2dDevice);
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: d2dFactory->CreateDevice failed hr=0x%08X", hr);
            return false;
        }

        hr = m_d2dDevice->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2dContext);
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: CreateDeviceContext failed hr=0x%08X", hr);
            return false;
        }
        WriteLaunchLog(L"InitD2D: D2D device + context ready");

        ComPtr<ABI::Windows::UI::Core::ICoreWindow> coreWnd(m_coreWindow);
        ABI::Windows::Foundation::Rect bounds;
        coreWnd->get_Bounds(&bounds);
        WriteLaunchLogF(L"InitD2D: bounds=%.1fx%.1f", bounds.Width, bounds.Height);

        ComPtr<IDXGIFactory2> dxgiFactory;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: CreateDXGIFactory1 failed hr=0x%08X", hr);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
        swapDesc.Width = (UINT)(bounds.Width);
        swapDesc.Height = (UINT)(bounds.Height);
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.Stereo = FALSE;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        hr = dxgiFactory->CreateSwapChainForCoreWindow(
            m_d3dDevice.Get(), (IUnknown*)coreWnd.Get(), &swapDesc,
            nullptr, &m_swapChain);
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: CreateSwapChainForCoreWindow failed hr=0x%08X (size=%ux%u)", hr, swapDesc.Width, swapDesc.Height);
            return false;
        }
        WriteLaunchLog(L"InitD2D: swap chain created");

        ComPtr<IDXGISurface> dxgiBackBuffer;
        hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: swap->GetBuffer(IDXGISurface) failed hr=0x%08X", hr);
            return false;
        }

        D2D1_BITMAP_PROPERTIES1 bitmapProps = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            96.0f, 96.0f);

        ComPtr<ID2D1Bitmap1> bitmap;
        hr = m_d2dContext->CreateBitmapFromDxgiSurface(
            dxgiBackBuffer.Get(), &bitmapProps, &bitmap);
        if (FAILED(hr)) {
            WriteLaunchLogF(L"InitD2D: CreateBitmapFromDxgiSurface failed hr=0x%08X", hr);
            return false;
        }

        m_d2dContext->SetTarget(bitmap.Get());

        float dpi = 96.0f;
        ComPtr<ID2D1Factory> factory;
        m_d2dContext->GetFactory(&factory);
dpi = 96.0f;
        m_d2dContext->SetDpi(dpi, dpi);

        m_consoleRendererInitialized = m_consoleRenderer.Initialize(m_d2dContext);
        if (m_consoleRendererInitialized) {
            m_consoleRenderer.SetBounds(bounds.Width, bounds.Height, 1.0f);
            WriteLaunchLog(L"InitD2D: console renderer initialized");
        } else {
            WriteLaunchLog(L"InitD2D: console renderer init FAILED");
        }

        return m_consoleRendererInitialized;
    }

    void StartSelectedInstance() {
        if (m_serverStarted || m_instances.empty()) return;
        const ServerInstance& inst = m_instances[m_instanceSel];
        m_ctx.workDir = inst.dir;
        m_ctx.serverJar = inst.jarPath;
        m_ctx.serverMainClass = inst.mainClass;
        m_runningLabel = inst.label;
        EnsureDirRecursive(inst.dir + L"\\tmp");
        WriteLaunchLogF(L"StartSelectedInstance: %ls dir=%ls", inst.label.c_str(), inst.dir.c_str());

        HANDLE t = CreateThread(nullptr, 0, ServerThreadEntry, &m_ctx, 0, nullptr);
        if (!t) {
            WriteLaunchLogF(L"StartSelectedInstance: CreateThread FAILED err=%u", GetLastError());
            m_launcherMsg = L"Failed to start server thread";
            return;
        }
        m_serverThread = t;
        m_serverStarted = true;
        m_consoleBuffer.Clear();
        m_screen = Screen::Console;
    }

    void EnterNewServer() {
        if (m_serverStarted) return;
        m_screen = Screen::NewServer;
        m_versions = DiscoverServerVersions(m_ctx.localStateDir, m_ctx.exeDir);
        m_versionSel = 0;
        m_launcherMsg.clear();
    }

    void BeginCreateInstance(ABI::Windows::UI::Core::ICoreWindow* sender) {
        if (m_versions.empty()) return;
        m_createFromIdx = m_versionSel;
        m_namingInstance = true;
        m_nameKbWasActive = false;
        m_instanceNameInput.clear();
        textentry::Show(L"", sender);
    }

    void FinishCreateInstance(const std::wstring& name) {
        if (m_createFromIdx < 0 || m_createFromIdx >= (int)m_versions.size()) return;
        std::wstring outId;
        bool ok = CreateInstance(m_ctx.localStateDir, name, m_versions[m_createFromIdx], outId);
        m_instances = DiscoverInstances(m_ctx.localStateDir);
        if (ok) {
            for (int i = 0; i < (int)m_instances.size(); ++i)
                if (m_instances[i].id == outId) { m_instanceSel = i; break; }
        }
        if (m_instanceSel >= (int)m_instances.size()) m_instanceSel = m_instances.empty() ? 0 : (int)m_instances.size() - 1;
        if (m_instanceSel < 0) m_instanceSel = 0;
        m_screen = Screen::Launcher;
        m_launcherMsg = ok ? L"Created server" : L"Could not create server";
    }

    void EnterDownloads() {
        if (m_serverStarted) return;
        m_screen = Screen::Downloads;
        m_dlSel = 0;
        m_dlProgress = -1;
        {
            std::lock_guard<std::mutex> lk(m_dlMutex);
            m_dlVersions.clear();
            m_dlStatus = L"Loading versions...";
        }
        m_dlBusy = true;
        std::thread([this]() {
            auto vers = FetchPaperVersions();
            std::lock_guard<std::mutex> lk(m_dlMutex);
            m_dlVersions = vers;
            m_dlStatus = vers.empty() ? L"Could not load versions (check network)" : L"";
            m_dlBusy = false;
        }).detach();
    }

    void BeginDeleteSelected() {
        if (m_instances.empty()) return;
        m_confirmDelete = true;
        m_launcherMsg = L"Delete " + m_instances[m_instanceSel].label + L"?   X confirm   B cancel";
    }

    void DeleteSelectedInstance() {
        m_confirmDelete = false;
        if (m_instances.empty()) return;
        ServerInstance inst = m_instances[m_instanceSel];
        bool ok = DeleteInstance(m_ctx.localStateDir, inst.id);
        m_instances = DiscoverInstances(m_ctx.localStateDir);
        if (m_instanceSel >= (int)m_instances.size()) m_instanceSel = (int)m_instances.size() - 1;
        if (m_instanceSel < 0) m_instanceSel = 0;
        m_launcherMsg = ok ? (L"Deleted " + inst.label) : (L"Delete failed: " + inst.label);
    }

    void DeleteSelectedPoolVersion() {
        if (m_versions.empty()) return;
        ServerVersion v = m_versions[m_versionSel];
        if (v.bundled) { m_launcherMsg = L"Cannot delete the bundled version"; return; }
        DeleteServerVersion(m_ctx.localStateDir, v.id);
        m_versions = DiscoverServerVersions(m_ctx.localStateDir, m_ctx.exeDir);
        if (m_versionSel >= (int)m_versions.size()) m_versionSel = (int)m_versions.size() - 1;
        if (m_versionSel < 0) m_versionSel = 0;
        m_launcherMsg = L"Removed " + v.label;
    }

    void StartDownloadSelected() {
        if (m_dlBusy) return;
        std::wstring ver;
        {
            std::lock_guard<std::mutex> lk(m_dlMutex);
            if (m_dlSel < 0 || m_dlSel >= (int)m_dlVersions.size()) return;
            ver = m_dlVersions[m_dlSel];
            m_dlStatus = L"Downloading " + ver + L"...";
        }
        m_dlBusy = true;
        m_dlProgress = 0;
        std::wstring localState = m_ctx.localStateDir;
        std::thread([this, ver, localState]() {
            std::wstring msg;
            bool ok = DownloadPaperVersion(localState, ver, &m_dlProgress, msg);
            {
                std::lock_guard<std::mutex> lk(m_dlMutex);
                m_dlStatus = msg;
            }
            if (ok) m_versionsDirty = true;
            m_dlProgress = -1;
            m_dlBusy = false;
        }).detach();
    }

    void OnKeyDown(ABI::Windows::UI::Core::ICoreWindow* sender, ABI::Windows::UI::Core::IKeyEventArgs* args) {
        ABI::Windows::System::VirtualKey virtualKey;
        args->get_VirtualKey(&virtualKey);

        ABI::Windows::UI::Core::CorePhysicalKeyStatus keyStatus;
        args->get_KeyStatus(&keyStatus);

        if (m_screen == Screen::Launcher) {
            int n = (int)m_instances.size();
            bool isX = (virtualKey == ABI::Windows::System::VirtualKey_X ||
                        virtualKey == ABI::Windows::System::VirtualKey_GamepadX);
            bool isBack = (virtualKey == ABI::Windows::System::VirtualKey_Escape ||
                           virtualKey == ABI::Windows::System::VirtualKey_GamepadB);

            if (m_confirmDelete) {
                if (isX) DeleteSelectedInstance();
                else if (isBack) { m_confirmDelete = false; m_launcherMsg.clear(); }
                return;
            }

            if (virtualKey == ABI::Windows::System::VirtualKey_Up ||
                virtualKey == ABI::Windows::System::VirtualKey_GamepadDPadUp ||
                virtualKey == ABI::Windows::System::VirtualKey_GamepadLeftThumbstickUp) {
                if (n > 0) m_instanceSel = (m_instanceSel + n - 1) % n;
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Down ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadDPadDown ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadLeftThumbstickDown) {
                if (n > 0) m_instanceSel = (m_instanceSel + 1) % n;
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Enter ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadA) {
                StartSelectedInstance();
            } else if (virtualKey == ABI::Windows::System::VirtualKey_GamepadY ||
                       virtualKey == ABI::Windows::System::VirtualKey_N) {
                EnterNewServer();
            } else if (isX) {
                if (n > 0) BeginDeleteSelected();
            }
            return;
        }

        if (m_screen == Screen::NewServer) {
            if (textentry::Active()) return;
            int n = (int)m_versions.size();
            bool isX = (virtualKey == ABI::Windows::System::VirtualKey_X ||
                        virtualKey == ABI::Windows::System::VirtualKey_GamepadX);
            if (virtualKey == ABI::Windows::System::VirtualKey_Up ||
                virtualKey == ABI::Windows::System::VirtualKey_GamepadDPadUp ||
                virtualKey == ABI::Windows::System::VirtualKey_GamepadLeftThumbstickUp) {
                if (n > 0) m_versionSel = (m_versionSel + n - 1) % n;
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Down ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadDPadDown ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadLeftThumbstickDown) {
                if (n > 0) m_versionSel = (m_versionSel + 1) % n;
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Enter ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadA) {
                BeginCreateInstance(sender);
            } else if (virtualKey == ABI::Windows::System::VirtualKey_GamepadY ||
                       virtualKey == ABI::Windows::System::VirtualKey_D) {
                EnterDownloads();
            } else if (isX) {
                DeleteSelectedPoolVersion();
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Escape ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadB) {
                m_screen = Screen::Launcher;
                m_launcherMsg.clear();
            }
            return;
        }

        if (m_screen == Screen::Downloads) {
            int n;
            { std::lock_guard<std::mutex> lk(m_dlMutex); n = (int)m_dlVersions.size(); }
            if (virtualKey == ABI::Windows::System::VirtualKey_Up ||
                virtualKey == ABI::Windows::System::VirtualKey_GamepadDPadUp ||
                virtualKey == ABI::Windows::System::VirtualKey_GamepadLeftThumbstickUp) {
                if (n > 0) m_dlSel = (m_dlSel + n - 1) % n;
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Down ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadDPadDown ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadLeftThumbstickDown) {
                if (n > 0) m_dlSel = (m_dlSel + 1) % n;
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Enter ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadA) {
                if (n == 0 && !m_dlBusy) EnterDownloads();
                else StartDownloadSelected();
            } else if (virtualKey == ABI::Windows::System::VirtualKey_GamepadY) {
                if (!m_dlBusy) EnterDownloads();
            } else if (virtualKey == ABI::Windows::System::VirtualKey_Escape ||
                       virtualKey == ABI::Windows::System::VirtualKey_GamepadB) {
                if (!m_dlBusy) m_screen = Screen::NewServer;
            }
            return;
        }

        if (textentry::Active()) return;

        bool stopped = m_serverStarted && !g_serverRunning;
        if (stopped) {
            if (virtualKey == ABI::Windows::System::VirtualKey_GamepadA ||
                virtualKey == ABI::Windows::System::VirtualKey_GamepadB ||
                virtualKey == ABI::Windows::System::VirtualKey_Enter) {
                appcontrol::RequestRestart();
            } else if (virtualKey == ABI::Windows::System::VirtualKey_PageUp) {
                m_consoleRenderer.ScrollUp(10);
            } else if (virtualKey == ABI::Windows::System::VirtualKey_PageDown) {
                m_consoleRenderer.ScrollDown(10);
            }
            return;
        }

        if (virtualKey == ABI::Windows::System::VirtualKey_GamepadA) {
            textentry::Show(m_commandInput, sender);
            return;
        }

        bool shiftHeld = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

        if (virtualKey == ABI::Windows::System::VirtualKey_Enter) {
            std::string cmd = WideToUtf8(m_commandInput) + "\n";
            WriteToStdin(cmd);

            m_consoleBuffer.Push(L"> " + m_commandInput);
            m_commandInput.clear();
            m_consoleRenderer.ResetScroll();
        } else if (virtualKey == ABI::Windows::System::VirtualKey_Back) {
            if (!m_commandInput.empty()) {
                m_commandInput.pop_back();
            }
        } else if (virtualKey == ABI::Windows::System::VirtualKey_PageUp) {
            m_consoleRenderer.ScrollUp(10);
        } else if (virtualKey == ABI::Windows::System::VirtualKey_PageDown) {
            m_consoleRenderer.ScrollDown(10);
        } else if (virtualKey == ABI::Windows::System::VirtualKey_Escape) {
            m_consoleRenderer.ResetScroll();
        } else {
            WCHAR ch = MapVirtualKeyToChar(virtualKey);
            if (ch >= 0x20 && ch <= 0x7E) {
                if (shiftHeld && ch >= 'a' && ch <= 'z') {
                    ch = (WCHAR)(ch - 32);
                } else if (shiftHeld) {
                    static const WCHAR shiftMap[] = {
                        0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
                        0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0, 0, 0, 0, 0, 0,
                        L' ', L'!', L'@', L'#', L'$', L'%', L'^', L'&',
                        L'*', L'(', L')', L'<', L':', L'_', L'>', L'?',
                        L')', L'!', L'@', L'#', L'$', L'%', L'^', L'&',
                        L'*', L'(', 0, 0, 0, 0, 0, 0
                    };
                    if (ch < ARRAYSIZE(shiftMap)) {
                        WCHAR shifted = shiftMap[ch];
                        if (shifted) ch = shifted;
                    }
                }
                m_commandInput += ch;
            }
        }

        if (virtualKey == ABI::Windows::System::VirtualKey_C &&
            keyStatus.IsMenuKeyDown) {
            g_shutdownRequested = true;
        }
    }

    static WCHAR MapVirtualKeyToChar(ABI::Windows::System::VirtualKey vk) {
        UINT scanCode = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
        if (scanCode == 0) return 0;

        BYTE keyState[256] = {};
        WCHAR buf[4] = {};
        int result = ToUnicode((UINT)vk, scanCode, keyState, buf, 4, 0);
        if (result > 0) return buf[0];
        return 0;
    }
    bool IsConsoleNoise(const std::wstring& line) {
        static const wchar_t* deprecation[] = {
            L"terminally deprecated method in java.lang.System",
            L"setSecurityManager has been called by",
            L"Please consider reporting this to the maintainers",
            L"setSecurityManager will be removed",
            L"command line option has enabled the Security Manager",
            L"Security Manager is deprecated",
        };
        for (auto* d : deprecation) {
            if (line.find(d) != std::wstring::npos) return true;
        }

        if (line.find(L"Exception stopping the server") != std::wstring::npos ||
            line.find(L"blocked by UWP host") != std::wstring::npos) {
            m_inTrapStack = true;
            return true;
        }
        if (m_inTrapStack) {
            size_t s = line.find_first_not_of(L" \t");
            std::wstring trimmed = (s == std::wstring::npos) ? L"" : line.substr(s);
            if (trimmed.empty() ||
                trimmed.rfind(L"at ", 0) == 0 ||
                trimmed.rfind(L"...", 0) == 0 ||
                trimmed.rfind(L"Caused by:", 0) == 0 ||
                trimmed.rfind(L"Suppressed:", 0) == 0) {
                return true;
            }
            m_inTrapStack = false;
        }
        return false;
    }

    ComPtr<ABI::Windows::UI::Core::ICoreWindow> m_coreWindow;
    bool m_windowClosed;
    bool m_windowVisible;

    ComPtr<ID3D11Device> m_d3dDevice;
    ComPtr<IDXGIDevice1> m_dxgiDevice;
    ComPtr<ID2D1Device> m_d2dDevice;
    ComPtr<ID2D1DeviceContext> m_d2dContext;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<IDWriteTextFormat> m_textFormat;
    ComPtr<ID2D1SolidColorBrush> m_textBrush;
    ComPtr<ID2D1SolidColorBrush> m_bgBrush;
    ComPtr<ID2D1SolidColorBrush> m_inputBrush;
    ComPtr<ID2D1SolidColorBrush> m_statusBrush;

    ConsoleRenderer m_consoleRenderer;
    bool m_consoleRendererInitialized = false;

    std::wstring m_commandInput;
    ConsoleRingBuffer m_consoleBuffer;

    ULONG m_refCount;

    HANDLE m_serverThread = nullptr;
    bool m_serverStarted;
    bool m_serverSuccess;
    bool m_inTrapStack = false;

    enum class Screen { Launcher, Console, Downloads, NewServer };
    Screen m_screen = Screen::Launcher;
    ServerThreadContext m_ctx{};
    std::vector<ServerVersion> m_versions;
    int m_versionSel = 0;
    std::vector<ServerInstance> m_instances;
    int m_instanceSel = 0;
    bool m_namingInstance = false;
    bool m_nameKbWasActive = false;
    std::wstring m_instanceNameInput;
    int m_createFromIdx = 0;
    std::wstring m_launcherMsg;
    std::atomic<bool> m_versionsDirty{false};
    bool m_confirmDelete = false;
    std::wstring m_runningLabel;
    bool m_keyboardWasActive = false;
    std::wstring m_webLine;

    std::vector<std::wstring> m_dlVersions;
    int m_dlSel = 0;
    std::wstring m_dlStatus;
    std::atomic<bool> m_dlBusy{false};
    std::atomic<int> m_dlProgress{-1};
    std::mutex m_dlMutex;

    EventRegistrationToken m_visToken;
    EventRegistrationToken m_closeToken;
};

class AppSource : public IFrameworkViewSource
{
public:
    AppSource() : m_refCount(1) {}

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IFrameworkViewSource) || riid == __uuidof(IUnknown) || riid == __uuidof(IInspectable)) {
            *ppv = static_cast<IFrameworkViewSource*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    STDMETHODIMP GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
    STDMETHODIMP GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE CreateView(IFrameworkView** view) override
    {
        auto app = new (std::nothrow) App();
        if (!app) return E_OUTOFMEMORY;
        *view = app;
        return S_OK;
    }

private:
    ULONG m_refCount;
};

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    RoInitialize(RO_INIT_MULTITHREADED);
    WriteLaunchLog(L"==== MC.Server launch ====");
    InstallProcessDiagnostics();
    WriteLaunchLogF(L"wWinMain: exeDir=%s", GetModuleDir().c_str());
    WriteLaunchLogF(L"wWinMain: localState=%s", ResolveLocalStateDir().c_str());

    ComPtr<ICoreApplication> coreApp;
    HRESULT hr = GetActivationFactory(
        HStringReference(RuntimeClass_Windows_ApplicationModel_Core_CoreApplication).Get(),
        &coreApp);
    if (FAILED(hr)) {
        WriteLaunchLogF(L"wWinMain: GetActivationFactory(CoreApplication) failed hr=0x%08X", hr);
        RoUninitialize();
        return 1;
    }
    {
        EventRegistrationToken suspToken, resToken, exitToken, enteredBgToken, leavingBgToken;
        auto suspHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::IEventHandler<
                ABI::Windows::ApplicationModel::SuspendingEventArgs*>>(
            [](IInspectable*, ABI::Windows::ApplicationModel::ISuspendingEventArgs* args) -> HRESULT {
                HandleSuspending(args);
                return S_OK;
            });
        auto resHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::IEventHandler<IInspectable*>>(
            [](IInspectable*, IInspectable*) -> HRESULT {
                HandleResuming();
                return S_OK;
            });
        auto exitHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::IEventHandler<IInspectable*>>(
            [](IInspectable*, IInspectable*) -> HRESULT {
                WriteLaunchLog(L"!! CoreApplication Exiting event fired !!");
                PersistLifecycleState(L"exiting");
                return S_OK;
            });
        auto enteredBgHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::IEventHandler<
                ABI::Windows::ApplicationModel::EnteredBackgroundEventArgs*>>(
            [](IInspectable*, ABI::Windows::ApplicationModel::IEnteredBackgroundEventArgs* args) -> HRESULT {
                HandleEnteredBackground(args);
                return S_OK;
            });
        auto leavingBgHandler = Microsoft::WRL::Callback<
            ABI::Windows::Foundation::IEventHandler<
                ABI::Windows::ApplicationModel::LeavingBackgroundEventArgs*>>(
            [](IInspectable*, ABI::Windows::ApplicationModel::ILeavingBackgroundEventArgs* args) -> HRESULT {
                HandleLeavingBackground(args);
                return S_OK;
            });
        if (suspHandler) coreApp->add_Suspending(suspHandler.Get(), &suspToken);
        if (resHandler) coreApp->add_Resuming(resHandler.Get(), &resToken);
        ComPtr<ICoreApplicationExit> coreAppExit;
        if (exitHandler && SUCCEEDED(coreApp.As(&coreAppExit)) && coreAppExit) {
            coreAppExit->add_Exiting(exitHandler.Get(), &exitToken);
        }
        ComPtr<ICoreApplication2> coreApp2;
        if (SUCCEEDED(coreApp.As(&coreApp2)) && coreApp2) {
            if (enteredBgHandler) coreApp2->add_EnteredBackground(enteredBgHandler.Get(), &enteredBgToken);
            if (leavingBgHandler) coreApp2->add_LeavingBackground(leavingBgHandler.Get(), &leavingBgToken);
        }
        WriteLaunchLog(L"wWinMain: lifecycle handlers registered");
    }

    auto source = ComPtr<AppSource>(new (std::nothrow) AppSource());
    if (source) {
        WriteLaunchLog(L"wWinMain: handing off to CoreApplication::Run");
        coreApp->Run(source.Get());
    } else {
        WriteLaunchLog(L"wWinMain: AppSource alloc failed");
    }

    WriteLaunchLog(L"wWinMain: CoreApplication::Run returned");
    RoUninitialize();
    return 0;
}
