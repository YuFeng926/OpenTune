#include "ScreenReaderAnnouncer.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <oleauto.h>
 #include <UIAutomation.h>
 #if JUCE_MSVC
  #pragma comment(lib, "oleaut32.lib") // SysAllocString / SysFreeString
 #endif
#endif

namespace OpenTune::a11y {

#if JUCE_WINDOWS
namespace {

// Values from UIAutomationCore.h (Windows 10 1709+). Declared here so the code does not
// depend on the Windows SDK version or _WIN32_WINNT.
constexpr int kNotificationKindOther = 4;              // NotificationKind_Other
constexpr int kProcessingImportantAll = 0;             // NotificationProcessing_ImportantAll
constexpr int kProcessingImportantMostRecent = 1;      // NotificationProcessing_ImportantMostRecent

using RaiseNotificationFn = HRESULT(WINAPI*)(IRawElementProviderSimple*, int, int, BSTR, BSTR);
using ClientsListeningFn = BOOL(WINAPI*)();
using RaisePropertyChangedFn = HRESULT(WINAPI*)(IRawElementProviderSimple*, int, VARIANT, VARIANT);

constexpr int kToggleStatePropertyId = 30086;          // UIA_ToggleToggleStatePropertyId

struct UiaFunctions
{
    UiaFunctions()
    {
        // Load at run time instead of linking: on old Windows the plugin should
        // just stay silent rather than fail to load in the host.
        if (auto* module = ::LoadLibraryW(L"UIAutomationCore.dll")) {
            raise = reinterpret_cast<RaiseNotificationFn>(::GetProcAddress(module, "UiaRaiseNotificationEvent"));
            listening = reinterpret_cast<ClientsListeningFn>(::GetProcAddress(module, "UiaClientsAreListening"));
            raiseProperty = reinterpret_cast<RaisePropertyChangedFn>(
                ::GetProcAddress(module, "UiaRaiseAutomationPropertyChangedEvent"));
        }
    }

    RaiseNotificationFn raise = nullptr;
    ClientsListeningFn listening = nullptr;
    RaisePropertyChangedFn raiseProperty = nullptr;
};

const UiaFunctions& uia()
{
    static const UiaFunctions instance;
    return instance;
}

// The caller releases the result. nullptr if the component is excluded from the
// accessibility tree or not on screen yet.
IRawElementProviderSimple* providerFor(juce::Component& source)
{
    auto* handler = source.getAccessibilityHandler();
    if (handler == nullptr)
        return nullptr;

    // On Windows JUCE's native implementation is a UI Automation provider COM object.
    auto* unknown = reinterpret_cast<IUnknown*>(handler->getNativeImplementation());
    if (unknown == nullptr)
        return nullptr;

    IRawElementProviderSimple* provider = nullptr;
    if (FAILED(unknown->QueryInterface(__uuidof(IRawElementProviderSimple),
                                       reinterpret_cast<void**>(&provider))))
        return nullptr;
    return provider;
}

} // namespace
#endif

void announce(juce::Component& source, const juce::String& text, SpeechQueue queue)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (text.isEmpty())
        return;

#if JUCE_WINDOWS
    // Do not use juce::AccessibilityHandler::postAnnouncement: on Windows JUCE speaks it
    // with a SAPI voice that bypasses the screen reader, and NVDA / JAWS never get it.
    const auto& functions = uia();
    if (functions.raise == nullptr)
        return;
    if (functions.listening != nullptr && !functions.listening())
        return; // no screen reader running

    auto* provider = providerFor(source);
    if (provider == nullptr)
        return;

    auto* message = ::SysAllocString(text.toWideCharPointer());
    auto* activity = ::SysAllocString(L"OpenTuneAnnouncement");

    functions.raise(provider,
                    kNotificationKindOther,
                    queue == SpeechQueue::replacePrevious ? kProcessingImportantMostRecent
                                                          : kProcessingImportantAll,
                    message,
                    activity);

    ::SysFreeString(message);
    ::SysFreeString(activity);
    provider->Release();
#else
    juce::ignoreUnused(source);
    // On macOS JUCE forwards this to VoiceOver as a regular announcement.
    juce::AccessibilityHandler::postAnnouncement(text,
        queue == SpeechQueue::replacePrevious ? juce::AccessibilityHandler::AnnouncementPriority::high
                                              : juce::AccessibilityHandler::AnnouncementPriority::medium);
#endif
}

void notifyToggleChanged(juce::Component& source, bool checked)
{
    JUCE_ASSERT_MESSAGE_THREAD

#if JUCE_WINDOWS
    const auto& functions = uia();
    if (functions.raiseProperty == nullptr)
        return;
    if (functions.listening != nullptr && !functions.listening())
        return; // no screen reader running

    auto* provider = providerFor(source);
    if (provider == nullptr)
        return;

    VARIANT oldValue;
    VARIANT newValue;
    ::VariantInit(&oldValue);
    ::VariantInit(&newValue);
    oldValue.vt = VT_I4;
    newValue.vt = VT_I4;
    oldValue.lVal = checked ? 0 : 1; // ToggleState_Off / ToggleState_On
    newValue.lVal = checked ? 1 : 0;

    functions.raiseProperty(provider, kToggleStatePropertyId, oldValue, newValue);
    provider->Release();
#else
    announce(source, checked ? "Checked" : "Not checked");
#endif
}

} // namespace OpenTune::a11y
