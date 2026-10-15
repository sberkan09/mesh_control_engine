// mce_platform: the Windows side of platform/screen_reader.h (Windows only). The game's window gets
// one UI Automation element, a minimal provider hosted by its HWND, and each announcement is a UIA
// notification event on it, which Narrator, NVDA and JAWS read out. SPI_GETSCREENREADER says
// whether one of them is running.

#include <SDL3/SDL.h>

#if defined(SDL_PLATFORM_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <atomic>

#include "mce/fixed_string.h"

namespace mce::platform::screen_reader {
namespace {

// The window's element: enough of IRawElementProviderSimple for a notification's source. UI
// Automation fills the rest (bounds, focus) from the host HWND.
class WindowElement final : public IRawElementProviderSimple {
public:
    explicit WindowElement(HWND hwnd) : hwnd_(hwnd) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *out = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        if (!out) return E_POINTER;
        *out = static_cast<ProviderOptions>(ProviderOptions_ServerSideProvider | ProviderOptions_UseComThreading);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* out) override {
        if (!out) return E_POINTER;
        VariantInit(out);
        if (id == UIA_ControlTypePropertyId) {
            out->vt = VT_I4;
            out->lVal = UIA_PaneControlTypeId;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (!out) return E_POINTER;
        return UiaHostProviderFromHwnd(hwnd_, out);
    }

    HWND hwnd() const { return hwnd_; }

private:
    HWND hwnd_;
    std::atomic<ULONG> refs_{1};
};

WindowElement* g_element = nullptr;

HWND hwnd_of(SDL_Window* window) {
    if (!window) return nullptr;
    return static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

}  // namespace

bool win32_running() {
    BOOL on = FALSE;
    return SystemParametersInfoW(SPI_GETSCREENREADER, 0, &on, 0) && on;
}

bool win32_announce(SDL_Window* window, StringView text) {
    const HWND hwnd = hwnd_of(window);
    if (!hwnd) return false;
    if (!g_element || g_element->hwnd() != hwnd) {
        if (g_element) g_element->Release();
        g_element = new WindowElement(hwnd);
    }
    if (!UiaClientsAreListening()) return false;  // no reader: nothing to tell
    // UTF-8 to UTF-16 BSTRs.
    const FixedString<512> z(text);
    const int wide = MultiByteToWideChar(CP_UTF8, 0, z.c_str(), -1, nullptr, 0);
    if (wide <= 1) return false;
    BSTR display = SysAllocStringLen(nullptr, static_cast<UINT>(wide - 1));
    if (!display) return false;
    MultiByteToWideChar(CP_UTF8, 0, z.c_str(), -1, display, wide);
    BSTR activity = SysAllocString(L"mce.ui");
    const HRESULT hr = UiaRaiseNotificationEvent(g_element, NotificationKind_ActionCompleted,
                                                 NotificationProcessing_ImportantMostRecent, display, activity);
    SysFreeString(activity);
    SysFreeString(display);
    return SUCCEEDED(hr);
}

}  // namespace mce::platform::screen_reader

#endif  // SDL_PLATFORM_WINDOWS
