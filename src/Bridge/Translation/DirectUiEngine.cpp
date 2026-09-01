#include "DirectUiEngine.h"

#include "SourceThreadAgent.h"
#include "ControlAdapters.h"
#include "../../Common/FluentShell.h"

#include <combaseapi.h>
#include <dwmapi.h>
#include <wincrypt.h>
#include <mscat.h>
#include <winver.h>
#include <wintrust.h>
#include <softpub.h>
#include <commctrl.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <thread>
#include <unordered_set>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace FluentShell::Bridge::Translation {
namespace {

using Microsoft::WRL::ComPtr;

constexpr DWORD kUiaDeadlineMs = 1600;
constexpr size_t kMaxTextChars = 4096;

bool Fail(std::wstring& error, std::wstring_view value) {
    error.assign(value);
    return false;
}

std::wstring ClassName(HWND window) {
    wchar_t value[128]{};
    const int length = GetClassNameW(window, value, static_cast<int>(std::size(value)));
    return length > 0 ? std::wstring(value, static_cast<size_t>(length)) : std::wstring();
}

bool SameRect(const RECT& left, const RECT& right) noexcept {
    return EqualRect(&left, &right) != FALSE;
}

bool SameWindow(const DirectUiWindowEvidence& left, const DirectUiWindowEvidence& right) noexcept {
    return left.hwnd == right.hwnd && left.generation == right.generation &&
        SameRect(left.bounds, right.bounds) && left.style == right.style &&
        left.exStyle == right.exStyle && left.visible == right.visible &&
        left.enabled == right.enabled && left.tabIndex == right.tabIndex &&
        left.text == right.text && left.note == right.note &&
        left.controlId == right.controlId && left.dialogCode == right.dialogCode &&
        left.checked == right.checked && left.minimum == right.minimum &&
        left.maximum == right.maximum && left.position == right.position &&
        left.indeterminate == right.indeterminate;
}

bool SameProfileWindow(
    const DirectUiSlot& slot,
    const DirectUiWindowEvidence& left,
    const DirectUiWindowEvidence& right) noexcept {
    const bool styleChanged = left.style != right.style;
    auto normalized = right;
    normalized.style = left.style;
    if (styleChanged && slot.kind == ControlKind::Button)
        normalized.dialogCode = left.dialogCode;
    if (!SameWindow(left, normalized)) return false;
    if (!styleChanged) return true;
    const uint64_t alternateBits = static_cast<uint64_t>(slot.nativeStyleValue) ^
        static_cast<uint64_t>(slot.nativeStyleAlt);
    const auto admitted = [&](uint64_t style) {
        const uint64_t masked = style & slot.nativeStyleMask;
        return masked == slot.nativeStyleValue || masked == slot.nativeStyleAlt;
    };
    const uint32_t dialogVariation = left.dialogCode ^ right.dialogCode;
    const uint32_t defaultDialogBits = DLGC_DEFPUSHBUTTON | DLGC_UNDEFPUSHBUTTON;
    const uint32_t leftDefaultDialog = left.dialogCode & defaultDialogBits;
    const uint32_t rightDefaultDialog = right.dialogCode & defaultDialogBits;
    const bool dialogCodeMatches = slot.kind != ControlKind::Button ||
        ((dialogVariation & ~defaultDialogBits) == 0 &&
         (leftDefaultDialog == DLGC_DEFPUSHBUTTON ||
          leftDefaultDialog == DLGC_UNDEFPUSHBUTTON) &&
         (rightDefaultDialog == DLGC_DEFPUSHBUTTON ||
          rightDefaultDialog == DLGC_UNDEFPUSHBUTTON));
    return alternateBits != 0 && dialogCodeMatches &&
        ((left.style ^ right.style) & ~alternateBits) == 0 &&
        admitted(left.style) && admitted(right.style);
}

bool SameMovedWindow(
    const DirectUiWindowEvidence& before,
    const DirectUiWindowEvidence& after,
    int64_t deltaX,
    int64_t deltaY) noexcept {
    const bool moved =
        static_cast<int64_t>(before.bounds.left) + deltaX == after.bounds.left &&
        static_cast<int64_t>(before.bounds.top) + deltaY == after.bounds.top &&
        static_cast<int64_t>(before.bounds.right) + deltaX == after.bounds.right &&
        static_cast<int64_t>(before.bounds.bottom) + deltaY == after.bounds.bottom;
    return moved && before.hwnd == after.hwnd &&
        before.generation == after.generation && before.style == after.style &&
        before.exStyle == after.exStyle && before.visible == after.visible &&
        before.enabled == after.enabled && before.tabIndex == after.tabIndex &&
        before.text == after.text && before.note == after.note &&
        before.controlId == after.controlId &&
        before.dialogCode == after.dialogCode && before.checked == after.checked &&
        before.minimum == after.minimum && before.maximum == after.maximum &&
        before.position == after.position && before.indeterminate == after.indeterminate;
}

// Reads one HWND facet block. All reads happen on the source GUI thread, so
// the values are canonical the moment they return.
bool ReadWindowEvidence(
    SourceThreadAgent& agent,
    HWND window,
    bool isButton,
    DirectUiWindowEvidence& value,
    std::wstring& error,
    bool isProgress = false) {
    if (!window || !IsWindow(window) || !GetWindowRect(window, &value.bounds))
        return Fail(error, L"window identity or geometry is unavailable");
    value.hwnd = window;
    value.generation = agent.DirectUiWindowGeneration(window);
    value.style = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_STYLE));
    value.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    value.visible = IsWindowVisible(window) != FALSE;
    value.enabled = IsWindowEnabled(window) != FALSE;
    value.controlId = GetDlgCtrlID(window);
    value.dialogCode = static_cast<uint32_t>(SendMessageW(window, WM_GETDLGCODE, 0, 0));
    if (isButton) value.checked = SendMessageW(window, BM_GETCHECK, 0, 0) != 0;
    if (isProgress) {
        PBRANGE range{};
        SendMessageW(window, PBM_GETRANGE, FALSE, reinterpret_cast<LPARAM>(&range));
        value.minimum = range.iLow;
        value.maximum = range.iHigh;
        value.position = static_cast<int>(SendMessageW(window, PBM_GETPOS, 0, 0));
        value.indeterminate = (static_cast<DWORD>(value.style) & PBS_MARQUEE) != 0;
        if (value.maximum <= value.minimum || value.position < value.minimum ||
            value.position > value.maximum)
            return Fail(error, L"progress backing range or position is invalid");
    }
    const int textLength = GetWindowTextLengthW(window);
    if (textLength < 0 || textLength > static_cast<int>(kMaxTextChars))
        return Fail(error, L"window text exceeds bound");
    std::wstring text(static_cast<size_t>(textLength) + 1, L'\0');
    const int copied = GetWindowTextW(window, text.data(), static_cast<int>(text.size()));
    if (copied < 0) return Fail(error, L"window text is unavailable");
    text.resize(static_cast<size_t>(copied));
    value.text = std::move(text);
    // BCM_GETNOTE only answers on command-link buttons; it is a no-op message
    // for every other class, so asking is always safe.
    if (isButton) {
        const LRESULT noteLength = SendMessageW(window, BCM_GETNOTELENGTH, 0, 0);
        if (noteLength > 0 && noteLength <= static_cast<LRESULT>(kMaxTextChars)) {
            std::wstring note(static_cast<size_t>(noteLength) + 1, L'\0');
            DWORD noteCapacity = static_cast<DWORD>(note.size());
            if (SendMessageW(window, BCM_GETNOTE,
                    reinterpret_cast<WPARAM>(&noteCapacity),
                    reinterpret_cast<LPARAM>(note.data()))) {
                note.resize(static_cast<size_t>(
                    std::find(note.begin(), note.end(), L'\0') - note.begin()));
                value.note = std::move(note);
            }
        }
    }
    return value.generation != 0 || Fail(error, L"window generation is unavailable");
}

bool ReadBasicWindowEvidence(
    SourceThreadAgent& agent,
    HWND window,
    DirectUiWindowEvidence& value,
    std::wstring& error) {
    if (!window || !IsWindow(window) || !GetWindowRect(window, &value.bounds))
        return Fail(error, L"window identity or geometry is unavailable");
    value.hwnd = window;
    value.generation = agent.DirectUiWindowGeneration(window);
    value.style = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_STYLE));
    value.exStyle = static_cast<uint64_t>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    value.visible = IsWindowVisible(window) != FALSE;
    value.enabled = IsWindowEnabled(window) != FALSE;
    value.controlId = GetDlgCtrlID(window);
    return value.generation != 0 || Fail(error, L"window generation is unavailable");
}

bool CaptureDirectUiBootstrapCore(
    SourceThreadAgent& agent,
    DirectUiBootstrapEvidence& evidence,
    std::wstring& error) {
    const HWND root = agent.Root();
    DWORD rootProcess = 0;
    const DWORD rootThread = GetWindowThreadProcessId(root, &rootProcess);
    if (!rootThread || rootThread != agent.ThreadId() || rootProcess != GetCurrentProcessId())
        return Fail(error, L"bootstrap: root thread or process identity mismatch");

    evidence = {};
    evidence.native.mutationEpoch = agent.MutationEpoch();
    DWORD cloakReasons = 0;
    if (FAILED(DwmGetWindowAttribute(root, DWMWA_CLOAKED,
            &cloakReasons, sizeof(cloakReasons))))
        return Fail(error, L"bootstrap: native cloak state is unavailable");
    evidence.native.cloaked = (cloakReasons & DWM_CLOAKED_APP) != 0;
    evidence.native.dpi = GetDpiForWindow(root);
    if (!evidence.native.dpi ||
        !ReadWindowEvidence(agent, root, false, evidence.native.root, error)) return false;
    evidence.native.ownerHwnd = GetWindow(root, GW_OWNER);
    evidence.native.title = evidence.native.root.text;
    if (!GetClientRect(root, &evidence.native.clientBounds))
        return Fail(error, L"bootstrap: root client geometry unavailable");
    evidence.native.clientOriginScreen = { 0, 0 };
    if (!ClientToScreen(root, &evidence.native.clientOriginScreen))
        return Fail(error, L"bootstrap: root client origin unavailable");

    std::vector<HWND> descendants;
    EnumChildWindows(root, [](HWND window, LPARAM raw) -> BOOL {
        auto& values = *reinterpret_cast<std::vector<HWND>*>(raw);
        values.push_back(window);
        return values.size() <= 256;
    }, reinterpret_cast<LPARAM>(&descendants));
    if (descendants.empty() || descendants.size() > 256)
        return Fail(error, L"bootstrap: descendant count is outside the generic bound");

    for (HWND child : descendants) {
        DWORD process = 0;
        if (GetWindowThreadProcessId(child, &process) != rootThread || process != rootProcess)
            return Fail(error, L"bootstrap: descendant escaped the source thread/process");
        DirectUiBootstrapWindowEvidence value;
        value.parent = GetParent(child);
        value.className = ClassName(child);
        if (value.className.empty() ||
            !ReadBasicWindowEvidence(agent, child, value.window, error)) return false;
        if (FluentShell::EqualsIgnoreCase(value.className, L"DirectUIHWND") &&
            value.window.visible && value.parent == root) {
            if (evidence.native.directUi.hwnd ||
                !ReadWindowEvidence(agent, child, false, evidence.native.directUi, error))
                return Fail(error, L"bootstrap: DirectUI anchor is missing or ambiguous");
        }
        if (FluentShell::EqualsIgnoreCase(value.className, L"#32770") &&
            value.window.visible) evidence.native.pageHosts.push_back(child);
        if (FluentShell::EqualsIgnoreCase(value.className, L"Static") &&
            value.window.visible) evidence.native.pageStatics.push_back(child);
        evidence.descendants.push_back(std::move(value));
    }
    if (!evidence.native.directUi.hwnd)
        return Fail(error, L"bootstrap: one visible root DirectUI anchor is required");
    evidence.native.mutationEpoch = agent.MutationEpoch();
    evidence.native.lastMutationHwnd = agent.LastMutationHwnd();
    evidence.native.lastMutationMessage = agent.LastMutationMessage();
    return true;
}

std::wstring CanonicalPath(std::wstring_view path) {
    const std::wstring owned(path);
    HANDLE file = CreateFileW(owned.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::wstring result(32768, L'\0');
    const DWORD length = GetFinalPathNameByHandleW(
        file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (length == 0 || length >= result.size()) return {};
    result.resize(length);
    if (result.starts_with(L"\\\\?\\")) result.erase(0, 4);
    return result;
}

bool ExactFileVersion(std::wstring_view path, const DirectUiWindowProfile& profile) {
    const std::wstring owned(path);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(owned.c_str(), &ignored);
    if (!size || size > 1024 * 1024) return false;
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(owned.c_str(), 0, size, data.data())) return false;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
        !info || infoSize < sizeof(*info) || info->dwSignature != VS_FFI_SIGNATURE) return false;
    return HIWORD(info->dwFileVersionMS) == profile.fileVersion[0] &&
        LOWORD(info->dwFileVersionMS) == profile.fileVersion[1] &&
        HIWORD(info->dwFileVersionLS) == profile.fileVersion[2] &&
        LOWORD(info->dwFileVersionLS) == profile.fileVersion[3];
}


std::wstring BstrValue(BSTR raw) {
    std::wstring value = raw ? std::wstring(raw, SysStringLen(raw)) : L"";
    if (raw) SysFreeString(raw);
    return value;
}

// Accepts a verified certificate chain when any certificate in it names the
// Microsoft Windows / Microsoft Corporation signer.
bool ChainNamesMicrosoftWindows(const CRYPT_PROVIDER_DATA* provider) {
    if (!provider) return false;
    const auto* signer = WTHelperGetProvSignerFromChain(
        const_cast<CRYPT_PROVIDER_DATA*>(provider), 0, FALSE, 0);
    if (!signer) return false;
    for (DWORD index = 0; index < signer->csCertChain; ++index) {
        const auto certificate = signer->pasCertChain[index].pCert;
        if (!certificate) continue;
        const DWORD length = CertGetNameStringW(certificate,
            CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
        if (length <= 1 || length > 1024) continue;
        std::wstring name(length, L'\0');
        if (CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE,
                0, nullptr, name.data(), length) == length) {
            name.resize(length - 1);
            if (IsMicrosoftWindowsSignerName(name)) return true;
        }
    }
    return false;
}

// Runs one WinVerifyTrust pass and reports whether the verified chain names
// the Microsoft Windows signer. State is always closed.
bool VerifyTrustOnce(
    WINTRUST_DATA& trust,
    GUID& policy,
    bool& microsoftSigned,
    LONG& status) {
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    status = WinVerifyTrust(nullptr, &policy, &trust);
    microsoftSigned = false;
    if (status == ERROR_SUCCESS && trust.hWVTStateData) {
        const auto* provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        microsoftSigned = ChainNamesMicrosoftWindows(provider);
    }
    const LONG closeStatus = [&]() {
        WINTRUST_DATA closing = trust;
        closing.dwStateAction = WTD_STATEACTION_CLOSE;
        return WinVerifyTrust(nullptr, &policy, &closing);
    }();
    (void)closeStatus;
    return status == ERROR_SUCCESS;
}

// Verifies a System32 executable that may be embedded-signed or catalog-signed.
// Catalog-signed binaries (the common case for inbox Windows tools) need an
// explicit catalog lookup: enumerate the security catalogs that contain the
// file's hash and verify the signature on each until one names Microsoft.
bool VerifyMicrosoftSignature(std::wstring_view path, std::wstring& error) {
    const std::wstring owned(path);

    // First try embedded Authenticode.
    WINTRUST_FILE_INFO file{ sizeof(file) };
    file.pcwszFilePath = owned.c_str();
    WINTRUST_DATA trust{ sizeof(trust) };
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_REVOCATION_CHECK_NONE;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = 0;
    bool microsoftSigned = false;
    if (VerifyTrustOnce(trust, policy, microsoftSigned, status)) {
        if (microsoftSigned) return true;
        // Embedded signature verified but the signer is not Microsoft; that is
        // a definitive answer, not a catalog case.
        return Fail(error, L"process: trusted signature has no Microsoft Windows signer");
    }

    // Catalog lookup: find catalogs whose members hash to this file.
    HCATADMIN admin = nullptr;
    if (!CryptCATAdminAcquireContext(&admin, nullptr, 0)) {
        return Fail(error, L"process: catalog administrator context unavailable");
    }
    bool verified = false;
    HANDLE fileHandle = INVALID_HANDLE_VALUE;
    do {
        fileHandle = CreateFileW(owned.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fileHandle == INVALID_HANDLE_VALUE) break;
        BYTE hash[64]{};
        DWORD hashSize = sizeof(hash);
        if (!CryptCATAdminCalcHashFromFileHandle(fileHandle, &hashSize, hash, 0)) {
            break;
        }
        std::wstring tag(hashSize * 2, L'\0');
        for (DWORD index = 0; index < hashSize; ++index) {
            swprintf_s(&tag[index * 2], 3, L"%02X", hash[index]);
        }

        // Enumerate every catalog containing this member hash and verify each.
        // The member file handle lets WinVerifyTrust recompute the hash itself,
        // which sidesteps tag-encoding differences between catalog versions.
        HCATINFO current = nullptr;
        while (!verified && (current = CryptCATAdminEnumCatalogFromHash(
                   admin, hash, hashSize, 0, &current)) != nullptr) {
            CATALOG_INFO foundInfo{};
            foundInfo.cbStruct = sizeof(foundInfo);
            if (CryptCATCatalogInfoFromContext(current, &foundInfo, 0)) {
                WINTRUST_DATA catalogTrust{ sizeof(catalogTrust) };
                WINTRUST_CATALOG_INFO catalogChoice{ sizeof(catalogChoice) };
                catalogChoice.pcwszCatalogFilePath = foundInfo.wszCatalogFile;
                catalogChoice.pcwszMemberFilePath = owned.c_str();
                catalogChoice.pcwszMemberTag = tag.c_str();
                catalogChoice.hMemberFile = fileHandle;
                catalogChoice.pbCalculatedFileHash = hash;
                catalogChoice.cbCalculatedFileHash = hashSize;
                catalogTrust.dwUIChoice = WTD_UI_NONE;
                catalogTrust.fdwRevocationChecks = WTD_REVOKE_NONE;
                catalogTrust.dwUnionChoice = WTD_CHOICE_CATALOG;
                catalogTrust.pCatalog = &catalogChoice;
                catalogTrust.dwProvFlags = WTD_REVOCATION_CHECK_NONE;
                LONG catalogStatus = 0;
                bool catalogMicrosoft = false;
                if (VerifyTrustOnce(catalogTrust, policy, catalogMicrosoft, catalogStatus) &&
                    catalogMicrosoft) {
                    verified = true;
                }
            }
        }
        // Enumeration releases the previous context when it advances. If we
        // stopped early after a successful verification, release the current
        // context explicitly as required by the catalog API contract.
        if (current) CryptCATAdminReleaseCatalogContext(admin, current, 0);
    } while (false);
    if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
    CryptCATAdminReleaseContext(admin, 0);
    if (!verified) {
        return Fail(error, L"process: catalog signature has no Microsoft Windows signer");
    }
    return true;
}

bool RuntimeId(IUIAutomationElement* element, std::vector<int>& result) {
    SAFEARRAY* raw = nullptr;
    if (FAILED(element->GetRuntimeId(&raw)) || !raw) return false;
    LONG lower = 0;
    LONG upper = -1;
    const bool bounded = SafeArrayGetDim(raw) == 1 &&
        SUCCEEDED(SafeArrayGetLBound(raw, 1, &lower)) &&
        SUCCEEDED(SafeArrayGetUBound(raw, 1, &upper)) && upper >= lower && upper - lower < 32;
    if (bounded) {
        result.reserve(static_cast<size_t>(upper - lower + 1));
        for (LONG index = lower; index <= upper; ++index) {
            int value = 0;
            if (FAILED(SafeArrayGetElement(raw, &index, &value))) {
                result.clear();
                break;
            }
            result.push_back(value);
        }
    }
    SafeArrayDestroy(raw);
    return bounded && !result.empty();
}

bool ReadSemantic(
    IUIAutomationElement* element,
    bool allowOffscreen,
    DirectUiSemanticEvidence& value) {
    BSTR raw = nullptr;
    if (FAILED(element->get_CurrentAutomationId(&raw))) return false;
    value.semanticKey = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentName(&raw))) return false;
    value.name = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentHelpText(&raw))) return false;
    value.helpText = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentAccessKey(&raw))) return false;
    value.accessKey = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentClassName(&raw))) return false;
    value.className = BstrValue(raw);
    raw = nullptr;
    if (FAILED(element->get_CurrentFrameworkId(&raw))) return false;
    value.frameworkId = BstrValue(raw);
    std::wstring normalized;
    if (!NormalizeDirectUiEvidenceText(value.semanticKey, normalized)) return false;
    value.semanticKey = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.name, normalized)) return false;
    value.name = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.helpText, normalized)) return false;
    value.helpText = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.accessKey, normalized)) return false;
    value.accessKey = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.className, normalized)) return false;
    value.className = std::move(normalized);
    if (!NormalizeDirectUiEvidenceText(value.frameworkId, normalized)) return false;
    value.frameworkId = std::move(normalized);
    UIA_HWND hwnd = nullptr;
    BOOL focusable = FALSE;
    BOOL enabled = FALSE;
    BOOL offscreen = TRUE;
    CONTROLTYPEID type = 0;
    RECT rect{};
    if (FAILED(element->get_CurrentNativeWindowHandle(&hwnd)) ||
        FAILED(element->get_CurrentControlType(&type)) ||
        FAILED(element->get_CurrentIsKeyboardFocusable(&focusable)) ||
        FAILED(element->get_CurrentIsEnabled(&enabled)) ||
        FAILED(element->get_CurrentIsOffscreen(&offscreen)) ||
        FAILED(element->get_CurrentBoundingRectangle(&rect)) ||
        (offscreen && !allowOffscreen)) return false;
    value.backingHwnd = reinterpret_cast<HWND>(hwnd);
    value.controlType = type;
    value.focusable = focusable != FALSE;
    value.enabled = enabled != FALSE;
    value.offscreen = offscreen != FALSE;
    value.bounds = rect;
    return RuntimeId(element, value.runtimeId);
}

bool ReadPatterns(IUIAutomationElement* element, DirectUiSemanticEvidence& semantic) {
    semantic.actionable = false;
    semantic.patternMask = DirectUiPatternNone;
    struct PatternProperty final { PROPERTYID property; uint32_t bit; };
    constexpr PatternProperty properties[] = {
        { UIA_IsInvokePatternAvailablePropertyId, DirectUiPatternInvoke },
        { UIA_IsTogglePatternAvailablePropertyId, DirectUiPatternToggle },
        { UIA_IsExpandCollapsePatternAvailablePropertyId, DirectUiPatternExpandCollapse },
        { UIA_IsSelectionItemPatternAvailablePropertyId, DirectUiPatternSelectionItem },
    };
    for (const auto& entry : properties) {
        VARIANT value{};
        VariantInit(&value);
        const HRESULT result = element->GetCurrentPropertyValue(entry.property, &value);
        if (FAILED(result) || value.vt != VT_BOOL) {
            VariantClear(&value);
            return false;
        }
        if (value.boolVal == VARIANT_TRUE) {
            semantic.patternMask |= entry.bit;
            semantic.actionable = true;
        }
        VariantClear(&value);
    }

    if ((semantic.patternMask & DirectUiPatternToggle) != 0) {
        ComPtr<IUIAutomationTogglePattern> toggle;
        ToggleState state = ToggleState_Indeterminate;
        if (FAILED(element->GetCurrentPatternAs(
                UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) || !toggle ||
            FAILED(toggle->get_CurrentToggleState(&state))) return false;
        semantic.toggleState = static_cast<int>(state);
    }

    // DirectUI exposes a read-only Value pattern on presentation-only Image
    // and Text elements. Availability alone is therefore not an action; only
    // a writable Value pattern can mutate canonical state.
    VARIANT available{};
    VariantInit(&available);
    const HRESULT valueResult = element->GetCurrentPropertyValue(
        UIA_IsValuePatternAvailablePropertyId, &available);
    if (FAILED(valueResult) || available.vt != VT_BOOL) {
        VariantClear(&available);
        return false;
    }
    if (available.boolVal == VARIANT_TRUE) {
        semantic.patternMask |= DirectUiPatternValue;
        ComPtr<IUIAutomationValuePattern> valuePattern;
        BOOL readOnly = TRUE;
        if (FAILED(element->GetCurrentPatternAs(
                UIA_ValuePatternId, IID_PPV_ARGS(&valuePattern))) ||
            !valuePattern || FAILED(valuePattern->get_CurrentIsReadOnly(&readOnly))) {
            VariantClear(&available);
            return false;
        }
        semantic.valueReadOnly = readOnly != FALSE;
        semantic.actionable = semantic.actionable || readOnly == FALSE;
    }
    VariantClear(&available);
    return true;
}

// Root nonclient chrome (TitleBar, SystemMenuBar and its items) belongs to the
// OS window frame, not to the projected page. The filter is an exact-shape
// predicate, never a blanket ignore: an unexpected focusable/actionable
// element outside these signatures still rejects the surface.
bool IsRootNonClientChrome(const DirectUiSemanticEvidence& semantic) {
    const bool bare = semantic.backingHwnd == nullptr && semantic.className.empty() &&
        semantic.frameworkId.empty();
    if (!bare) return false;
    if (semantic.controlType == UIA_TitleBarControlTypeId && semantic.semanticKey == L"TitleBar")
        return true;
    if (semantic.controlType == UIA_MenuBarControlTypeId &&
        semantic.semanticKey == L"SystemMenuBar" && !semantic.actionable)
        return true;
    if (semantic.controlType == UIA_ButtonControlTypeId && !semantic.focusable &&
        semantic.actionable &&
        (semantic.semanticKey == L"Close" || semantic.semanticKey == L"Minimize" ||
            semantic.semanticKey == L"Maximize" || semantic.semanticKey == L"Restore" ||
            semantic.semanticKey == L"Help"))
        return true;
    // System menu items are MenuItems parented under SystemMenuBar and always
    // sit outside the DirectUI host bounds.
    if (semantic.controlType == UIA_MenuItemControlTypeId &&
        semantic.semanticKey.empty())
        return true;
    return false;
}

bool CaptureUia(
    HWND rootHwnd,
    HWND directUiHwnd,
    DWORD processId,
    bool allowDeclaredOffscreen,
    const std::vector<std::wstring>& virtualAutomationIds,
    const std::vector<HWND>& backingWindows,
    bool retainAllSemantics,
    DirectUiUiaEvidence& evidence,
    std::wstring& error) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return Fail(error, L"MTA COM initialization failed");
    struct Uninitialize final { bool active; ~Uninitialize() { if (active) CoUninitialize(); } } scope{
        SUCCEEDED(initialized) };
    ComPtr<IUIAutomation> automation;
    HRESULT result = CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation));
    if (FAILED(result) || !automation) return Fail(error, L"UIA8 client creation failed");
    ComPtr<IUIAutomation6> bounded;
    if (FAILED(automation.As(&bounded)) || !bounded ||
        FAILED(bounded->put_ConnectionTimeout(900)) ||
        FAILED(bounded->put_TransactionTimeout(900)))
        return Fail(error, L"bounded UIA timeouts are unavailable");
    ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(rootHwnd, &root)) || !root)
        return Fail(error, L"root UIA provider is unavailable");
    int rootPid = 0;
    UIA_HWND rootBacking = nullptr;
    if (FAILED(root->get_CurrentProcessId(&rootPid)) || rootPid != static_cast<int>(processId) ||
        FAILED(root->get_CurrentNativeWindowHandle(&rootBacking)) ||
        reinterpret_cast<HWND>(rootBacking) != rootHwnd)
        return Fail(error, L"root UIA identity mismatch");
    evidence.rootHwnd = rootHwnd;
    ComPtr<IUIAutomationElement> directUi;
    int directUiPid = 0;
    UIA_HWND directUiBacking = nullptr;
    if (FAILED(automation->ElementFromHandle(directUiHwnd, &directUi)) || !directUi ||
        FAILED(directUi->get_CurrentProcessId(&directUiPid)) ||
        directUiPid != static_cast<int>(processId) ||
        FAILED(directUi->get_CurrentNativeWindowHandle(&directUiBacking)) ||
        reinterpret_cast<HWND>(directUiBacking) != directUiHwnd)
        return Fail(error, L"DirectUI anchor UIA identity mismatch");
    evidence.directUiHwnd = directUiHwnd;
    ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreateTrueCondition(&condition)) || !condition)
        return Fail(error, L"UIA descendant condition failed");
    ComPtr<IUIAutomationElementArray> descendants;
    if (FAILED(root->FindAll(TreeScope_Descendants, condition.Get(), &descendants)) || !descendants)
        return Fail(error, L"UIA descendant enumeration failed");
    int count = 0;
    if (FAILED(descendants->get_Length(&count)) || count < 1 || count > 64)
        return Fail(error, L"UIA descendant count is outside the exact bound");
    for (int index = 0; index < count; ++index) {
        ComPtr<IUIAutomationElement> element;
        if (FAILED(descendants->GetElement(index, &element)) || !element)
            return Fail(error, L"UIA descendant is unavailable");
        int pid = 0;
        BOOL offscreen = TRUE;
        if (FAILED(element->get_CurrentProcessId(&pid)) ||
            (pid != 0 && pid != static_cast<int>(processId)) ||
            FAILED(element->get_CurrentIsOffscreen(&offscreen)))
            return Fail(error, L"UIA descendant process or visibility is unavailable");
        if (offscreen && !allowDeclaredOffscreen) continue;
        DirectUiSemanticEvidence semantic;
        if (!ReadSemantic(element.Get(), allowDeclaredOffscreen, semantic))
            return Fail(error, L"visible UIA semantic evidence read failed");
        if (!ReadPatterns(element.Get(), semantic))
            return Fail(error, L"visible UIA action-pattern evidence read failed");
        const bool actionable = semantic.actionable || semantic.focusable ||
            semantic.controlType == UIA_ButtonControlTypeId ||
            semantic.controlType == UIA_HyperlinkControlTypeId ||
            semantic.controlType == UIA_MenuItemControlTypeId;
        const bool declaredVirtual = std::find(virtualAutomationIds.begin(),
            virtualAutomationIds.end(), semantic.semanticKey) != virtualAutomationIds.end();
        const bool declaredBacking = semantic.backingHwnd &&
            std::find(backingWindows.begin(), backingWindows.end(),
                semantic.backingHwnd) != backingWindows.end();
        const bool retain = retainAllSemantics || declaredVirtual || declaredBacking ||
            (!semantic.offscreen && actionable);
        if (retain &&
            !IsRootNonClientChrome(semantic))
            evidence.semantics.push_back(std::move(semantic));
    }
    return true;
}

const DirectUiSemanticEvidence* FindSemanticByAutomationId(
    const DirectUiUiaEvidence& evidence,
    std::wstring_view automationId) noexcept {
    const auto found = std::find_if(evidence.semantics.begin(), evidence.semantics.end(),
        [&](const auto& value) { return value.semanticKey == automationId; });
    return found == evidence.semantics.end() ? nullptr : &*found;
}

const DirectUiSemanticEvidence* FindSemanticByBacking(
    const DirectUiUiaEvidence& evidence,
    HWND backing) noexcept {
    const auto found = std::find_if(evidence.semantics.begin(), evidence.semantics.end(),
        [&](const auto& value) { return value.backingHwnd == backing; });
    return found == evidence.semantics.end() ? nullptr : &*found;
}

bool RectNear(const RECT& left, const RECT& right, LONG tolerance = 2) noexcept {
    return std::abs(left.left - right.left) <= tolerance &&
        std::abs(left.top - right.top) <= tolerance &&
        std::abs(left.right - right.right) <= tolerance &&
        std::abs(left.bottom - right.bottom) <= tolerance;
}

bool Inside(const RECT& child, const RECT& parent) noexcept {
    return child.left >= parent.left && child.top >= parent.top &&
        child.right <= parent.right && child.bottom <= parent.bottom;
}

std::wstring DisplayText(std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L'&' && index + 1 < value.size()) {
            if (value[index + 1] != L'&') continue;
            ++index;
        }
        result.push_back(value[index]);
    }
    return result;
}

RECT ClientRectFor(const RECT& screen, const POINT& origin) noexcept {
    return { screen.left - origin.x, screen.top - origin.y,
        screen.right - origin.x, screen.bottom - origin.y };
}

bool LoadTrustedIconResource(
    std::wstring_view imagePath,
    int resourceId,
    int width,
    int height,
    ControlNode& node,
    std::wstring& error) {
    const std::wstring owned(imagePath);
    HMODULE resources = LoadLibraryExW(owned.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!resources) return Fail(error, L"resource: application image resource module unavailable");
    HICON icon = reinterpret_cast<HICON>(LoadImageW(resources, MAKEINTRESOURCEW(resourceId),
        IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    bool captured = false;
    if (icon) {
        captured = CaptureOwnedIconPixels(icon, node.imageWidth, node.imageHeight,
            node.imageFormat, node.imageData, error);
        DestroyIcon(icon);
    } else {
        error = L"resource: application icon resource is unavailable";
    }
    FreeLibrary(resources);
    return captured && node.imageWidth == static_cast<uint32_t>(width) &&
        node.imageHeight == static_cast<uint32_t>(height) ||
        Fail(error, L"resource: application icon did not resolve to owned pixels");
}

// Builds the composite renderer snapshot from verified evidence. Every node is
// emitted by profile slot order, so no two profiles can drift apart.
bool BuildCompositeSnapshot(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    std::wstring_view imagePath,
    const DirectUiNativeEvidence& native,
    const DirectUiUiaEvidence& uia,
    WindowSnapshot& snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>& bindings,
    std::wstring& error) {
    snapshot.surfaceKind = SurfaceKind::Window;
    snapshot.modal = false;
    snapshot.canCancel = std::any_of(
        profile.slots, profile.slots + profile.slotCount,
        [](const DirectUiSlot& slot) { return slot.cancel; });
    snapshot.icon = L"none";
    snapshot.generation = agent.Generation();
    snapshot.nativeHwnd = native.root.hwnd;
    snapshot.ownerHwnd = native.ownerHwnd;
    snapshot.title = native.title;
    snapshot.dpi = native.dpi;
    snapshot.bounds = native.root.bounds;
    snapshot.clientBounds = native.clientBounds;
    snapshot.windowStyle = native.root.style;
    snapshot.windowExStyle = native.root.exStyle;
    snapshot.visible = native.root.visible;
    snapshot.enabled = native.root.enabled;
    snapshot.state = L"normal";
    snapshot.showInTaskbar = native.ownerHwnd == nullptr ||
        (native.root.exStyle & WS_EX_APPWINDOW) != 0;
    snapshot.rtl = (native.root.exStyle & WS_EX_LAYOUTRTL) != 0;
    snapshot.adapterId = profile.adapterId;
    snapshot.pageId = profile.pageId;
    snapshot.menu.clear();
    snapshot.nodes.clear();
    bindings.clear();

    for (size_t index = 0; index < profile.slotCount; ++index) {
        const DirectUiSlot& slot = profile.slots[index];
        if (!slot.project) continue;
        ControlNode node;
        node.nodeId = DirectUiSemanticNodeId(profile.adapterId, slot.semanticKey);
        node.kind = slot.kind;
        node.zIndex = static_cast<int>(snapshot.nodes.size());
        node.adapterId = profile.adapterId;
        node.pageId = profile.pageId;
        node.semanticKey = slot.semanticKey;
        node.sourceKind = slot.virtualSource ? L"uiaVirtual" : L"nativeBacking";
        node.presentationVariant = slot.presentationVariant;
        node.helpText = L"";
        node.accessKey = L"";
        node.isDefault = slot.defaultButton;
        if (slot.action == DirectUiAction::None) {
            node.supportedActions = {};
        } else if (slot.action == DirectUiAction::HandoffClick) {
            node.supportedActions = { L"invoke" };
        } else {
            node.supportedActions = { L"setCheck" };
        }

        if (slot.virtualSource) {
            const auto* semantic = FindSemanticByAutomationId(uia,
                slot.uiaAutomationId.empty() ? slot.semanticKey : slot.uiaAutomationId);
            if (!semantic) return Fail(error, L"snapshot: projected virtual slot is missing");
            node.generation = native.root.generation;
            node.tabIndex = slot.tabIndex;
            node.rect = ClientRectFor(semantic->bounds, native.clientOriginScreen);
            node.visible = true;
            node.enabled = native.root.enabled && native.directUi.enabled &&
                semantic->enabled;
            node.tabStop = node.enabled && slot.uiaFocusable;
            node.text = semantic->name;
            node.automationName = semantic->name;
            node.helpText = semantic->helpText;
            node.accessKey = semantic->accessKey;
            if (slot.kind == ControlKind::CheckBox && semantic->toggleState >= 0)
                node.checked = semantic->toggleState == ToggleState_On ? 1 : 0;
            if (slot.iconResourceId) {
                if (!LoadTrustedIconResource(imagePath, slot.iconResourceId,
                        slot.iconWidth, slot.iconHeight, node, error)) return false;
            }
        } else {
            const DirectUiWindowEvidence& backing = native.slotWindows[index];
            const auto* semantic = FindSemanticByBacking(uia, backing.hwnd);
            if (!semantic) return Fail(error, L"snapshot: backing UIA semantic is missing");
            node.generation = backing.generation;
            node.hwnd = backing.hwnd;
            node.controlId = backing.controlId;
            node.tabIndex = backing.tabIndex;
            node.rect = ClientRectFor(backing.bounds, native.clientOriginScreen);
            node.style = backing.style;
            node.exStyle = backing.exStyle;
            node.visible = backing.visible;
            node.enabled = native.root.enabled && native.directUi.enabled &&
                backing.enabled && semantic->enabled;
            node.tabStop = node.enabled && backing.tabIndex >= 0;
            node.dialogCode = backing.dialogCode;
            node.text = backing.text.empty() ? semantic->name : backing.text;
            node.automationName = semantic->name;
            node.helpText = semantic->helpText.empty() ? backing.note : semantic->helpText;
            node.accessKey = semantic->accessKey;
            node.checked = backing.checked ? 1 : 0;
            node.minimum = backing.minimum;
            node.maximum = backing.maximum;
            node.position = backing.position;
            node.indeterminate = backing.indeterminate;
            bindings.emplace(node.nodeId,
                DirectUiActionBinding{ backing.hwnd, backing.generation, index,
                    slot.action, slot.cancel });
        }
        snapshot.nodes.push_back(std::move(node));
    }
    return true;
}

bool BuildGenericSemanticProfile(
    const DirectUiBootstrapEvidence& native,
    DirectUiUiaEvidence& uia,
    std::wstring_view imagePath,
    std::shared_ptr<DirectUiOwnedProfile>& owned,
    std::wstring& error) {
    if (uia.semantics.empty() || uia.semantics.size() > 256)
        return Fail(error, L"generic U: semantic count is outside the capability bound");
    auto result = std::make_shared<DirectUiOwnedProfile>();
    const auto own = [&](std::wstring value) -> std::wstring_view {
        result->strings.push_back(std::move(value));
        return result->strings.back();
    };

    for (const auto& descendant : native.descendants) {
        auto found = std::find_if(result->implementationClasses.begin(),
            result->implementationClasses.end(), [&](const auto& contract) {
                return contract.visible == descendant.window.visible &&
                    FluentShell::EqualsIgnoreCase(contract.className, descendant.className);
            });
        if (found == result->implementationClasses.end()) {
            result->implementationClasses.push_back(
                { own(descendant.className), descendant.window.visible, 1 });
        } else {
            ++found->count;
        }
    }

    std::unordered_set<std::wstring> virtualIds;
    std::unordered_set<HWND> backingHwnds;
    std::vector<DirectUiSemanticEvidence> admittedSemantics;
    admittedSemantics.reserve(uia.semantics.size());
    int nextTabIndex = 0;
    size_t semanticOrdinal = 0;
    for (const auto& semantic : uia.semantics) {
        const auto describeSemantic = [&] {
            return L" (type=" + std::to_wstring(semantic.controlType) +
                L", name=" + semantic.name + L", id=" + semantic.semanticKey +
                L", class=" + semantic.className + L", framework=" + semantic.frameworkId +
                L", hwnd=" + std::to_wstring(reinterpret_cast<uintptr_t>(semantic.backingHwnd)) +
                L", patterns=" +
                std::to_wstring(semantic.patternMask) + L", focusable=" +
                std::to_wstring(semantic.focusable) + L", enabled=" +
                std::to_wstring(semantic.enabled) + L", actionable=" +
                std::to_wstring(semantic.actionable) + L")";
        };
        const uint32_t behavioralPatternMask = semantic.patternMask &
            ~(semantic.valueReadOnly ? DirectUiPatternValue : DirectUiPatternNone);
        const bool insideRoot = Inside(semantic.bounds, native.native.root.bounds);
        const bool insideAnchor = Inside(semantic.bounds, native.native.directUi.bounds);
        if (!insideRoot || semantic.offscreen)
            return Fail(error, L"generic U: semantic geometry escaped the admitted root");

        DirectUiSlot slot;
        slot.semanticKey = own(L"semantic." + std::to_wstring(semanticOrdinal++));
        slot.uiaControlType = semantic.controlType;
        slot.uiaAutomationId = own(semantic.semanticKey);
        slot.uiaClassName = own(semantic.className);
        slot.uiaFrameworkId = own(semantic.frameworkId);
        slot.uiaEnabled = semantic.enabled;
        slot.uiaFocusable = semantic.focusable;
        slot.uiaActionable = semantic.actionable;
        slot.uiaBoundsScope = insideAnchor
            ? DirectUiBoundsScope::Anchor : DirectUiBoundsScope::Root;
        slot.pinUiaPatterns = true;
        slot.uiaPatternMask = semantic.patternMask;
        slot.uiaToggleState = semantic.toggleState;

        const auto backing = std::find_if(native.descendants.begin(),
            native.descendants.end(), [&](const auto& candidate) {
                return candidate.window.hwnd == semantic.backingHwnd;
            });
        const bool standardBacking = backing != native.descendants.end() &&
            (FluentShell::EqualsIgnoreCase(backing->className, L"Button") ||
             FluentShell::EqualsIgnoreCase(backing->className, L"Static"));
        const bool progressBacking = backing != native.descendants.end() &&
            FluentShell::EqualsIgnoreCase(backing->className, PROGRESS_CLASSW);

        bool retain = true;
        switch (semantic.controlType) {
        case UIA_TextControlTypeId:
            if (semantic.focusable || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone ||
                (semantic.name.empty() &&
                    (backing == native.descendants.end() || semantic.semanticKey.empty())))
                return Fail(error, L"generic U: Text semantic exposes unsupported behavior" +
                    describeSemantic());
            // Empty native Static providers are layout/implementation evidence,
            // not visible text. Keep their identity in the contract without
            // manufacturing an unnamed WinUI text node.
            slot.project = !semantic.name.empty();
            slot.kind = ControlKind::StaticText;
            slot.presentationVariant = L"standard";
            break;
        case UIA_SeparatorControlTypeId:
            if (semantic.focusable || semantic.actionable ||
                semantic.patternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: Separator semantic is actionable" +
                    describeSemantic());
            slot.kind = ControlKind::Separator;
            slot.presentationVariant = L"standard";
            break;
        case UIA_ButtonControlTypeId:
            if (semantic.name.empty() ||
                behavioralPatternMask != DirectUiPatternInvoke ||
                (semantic.enabled && !semantic.focusable))
                return Fail(error, L"generic U: Button lacks an exact Invoke contract" +
                    describeSemantic());
            slot.kind = ControlKind::Button;
            slot.presentationVariant = standardBacking &&
                ((backing->window.style & BS_TYPEMASK) == BS_COMMANDLINK ||
                 (backing->window.style & BS_TYPEMASK) == BS_DEFCOMMANDLINK)
                ? L"commandLink" : L"standard";
            slot.action = semantic.enabled ? DirectUiAction::HandoffClick : DirectUiAction::None;
            if (semantic.enabled && !standardBacking)
                return Fail(error, L"generic U: virtual Invoke requires an isolated UIA action broker" +
                    describeSemantic());
            break;
        case UIA_CheckBoxControlTypeId:
            if (semantic.name.empty() || behavioralPatternMask != DirectUiPatternToggle ||
                (semantic.toggleState != ToggleState_Off &&
                 semantic.toggleState != ToggleState_On) ||
                (semantic.enabled && (!semantic.focusable || !standardBacking)))
                return Fail(error, L"generic U: CheckBox lacks a binary native-backed Toggle contract" +
                    describeSemantic());
            slot.kind = ControlKind::CheckBox;
            slot.presentationVariant = L"standard";
            slot.action = semantic.enabled ? DirectUiAction::ToggleCheck : DirectUiAction::None;
            break;
        case UIA_ProgressBarControlTypeId:
            if (semantic.name.empty() || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone || !progressBacking ||
                (backing->window.style & (PBS_VERTICAL | WS_TABSTOP)) != 0)
                return Fail(error, L"generic U: ProgressBar lacks a read-only native backing contract" +
                    describeSemantic());
            slot.kind = ControlKind::ProgressBar;
            slot.presentationVariant = L"standard";
            break;
        case UIA_ImageControlTypeId:
        case UIA_PaneControlTypeId:
        case UIA_GroupControlTypeId:
        case UIA_WindowControlTypeId: {
            // Some Win32/DirectUI providers put a read-only Value pattern on
            // structural roots. It cannot mutate canonical state and is still
            // pinned in the generated evidence contract when the element has
            // a stable identity.
            // A native-backed, nameless structural provider may advertise a
            // focus stop solely to delegate focus into its child controls.
            // It has no mutable pattern and is accounted but not projected.
            const bool delegatedStructuralFocus = semantic.focusable &&
                !semantic.actionable && behavioralPatternMask == DirectUiPatternNone &&
                backing != native.descendants.end() && semantic.name.empty() &&
                !semantic.semanticKey.empty();
            if ((semantic.focusable && !delegatedStructuralFocus) || semantic.actionable ||
                behavioralPatternMask != DirectUiPatternNone)
                return Fail(error, L"generic U: structural semantic is unexpectedly actionable" +
                    describeSemantic());
            slot.project = false;
            slot.kind = ControlKind::StaticText;
            slot.presentationVariant = L"standard";
            retain = !semantic.semanticKey.empty();
            break;
        }
        default:
            return Fail(error, L"generic U: UIA control type is outside the admitted semantic set" +
                describeSemantic());
        }

        if (!retain) continue;
        if ((!standardBacking && !progressBacking) || !slot.project ||
            (slot.kind != ControlKind::Button && slot.kind != ControlKind::CheckBox &&
             slot.kind != ControlKind::StaticText && slot.kind != ControlKind::ProgressBar)) {
            slot.virtualSource = true;
            if (semantic.semanticKey.empty() || !virtualIds.insert(semantic.semanticKey).second)
                return Fail(error, L"generic U: virtual semantic AutomationId is empty or duplicated");
        } else {
            slot.virtualSource = false;
            if (!backingHwnds.insert(backing->window.hwnd).second)
                return Fail(error, L"generic U: native backing is shared by multiple semantics");
            slot.nativeClass = own(backing->className);
            slot.nativeStyleMask = UINT64_MAX;
            slot.nativeStyleValue = static_cast<uint32_t>(backing->window.style);
            slot.nativeStyleAlt = slot.nativeStyleValue;
            if (slot.kind == ControlKind::Button) {
                const DWORD buttonType = slot.nativeStyleValue & BS_TYPEMASK;
                slot.defaultButton = buttonType == BS_DEFPUSHBUTTON ||
                    buttonType == BS_DEFCOMMANDLINK;
                if (buttonType == BS_PUSHBUTTON || buttonType == BS_DEFPUSHBUTTON) {
                    slot.nativeStyleAlt = (slot.nativeStyleValue & ~BS_TYPEMASK) |
                        (buttonType == BS_PUSHBUTTON ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON);
                } else if (buttonType == BS_COMMANDLINK || buttonType == BS_DEFCOMMANDLINK) {
                    slot.nativeStyleAlt = (slot.nativeStyleValue & ~BS_TYPEMASK) |
                        (buttonType == BS_COMMANDLINK ? BS_DEFCOMMANDLINK : BS_COMMANDLINK);
                }
            }
            slot.nativeControlId = backing->window.controlId;
        }
        if (slot.project && slot.action != DirectUiAction::None &&
            semantic.enabled && semantic.focusable)
            slot.tabIndex = nextTabIndex++;
        result->slots.push_back(slot);
        admittedSemantics.push_back(semantic);
    }
    if (result->slots.empty())
        return Fail(error, L"generic U: no projectable or accountable semantics remain");

    auto& profile = result->profile;
    profile.adapterId = L"microsoft.windows.directui.semantic.v1";
    profile.pageId = L"semantic-v1";
    profile.executableBasename = own(std::wstring(FluentShell::FileNameOf(imagePath)));
    profile.rootClass = own(ClassName(native.native.root.hwnd));
    profile.slots = result->slots.data();
    profile.slotCount = result->slots.size();
    profile.directUiOwnsTabOrder = true;
    profile.minDescendants = native.descendants.size();
    profile.maxDescendants = native.descendants.size();
    profile.implementationClasses = result->implementationClasses.data();
    profile.implementationClassCount = result->implementationClasses.size();
    profile.genericSemantic = true;
    uia.semantics = std::move(admittedSemantics);
    owned = std::move(result);
    return true;
}

bool CaptureGeneratedProfileEvidence(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DirectUiNativeEvidence& evidence,
    std::wstring& error) {
    DirectUiBootstrapEvidence bootstrap;
    if (!CaptureDirectUiBootstrapCore(agent, bootstrap, error)) return false;
    if (!FluentShell::EqualsIgnoreCase(ClassName(agent.Root()), profile.rootClass) ||
        bootstrap.descendants.size() < profile.minDescendants ||
        bootstrap.descendants.size() > profile.maxDescendants) {
        return Fail(error, L"generic A/B: root class or descendant count changed");
    }

    size_t accountedClasses = 0;
    for (size_t contractIndex = 0;
         contractIndex < profile.implementationClassCount; ++contractIndex) {
        const auto& contract = profile.implementationClasses[contractIndex];
        const auto count = static_cast<size_t>(std::count_if(
            bootstrap.descendants.begin(), bootstrap.descendants.end(),
            [&](const auto& candidate) {
                return candidate.window.visible == contract.visible &&
                    FluentShell::EqualsIgnoreCase(candidate.className, contract.className);
            }));
        if (count != contract.count)
            return Fail(error, L"generic A/B: implementation HWND inventory changed");
        accountedClasses += count;
    }
    if (accountedClasses != bootstrap.descendants.size())
        return Fail(error, L"generic A/B: implementation HWND class is outside the sealed inventory");

    evidence = std::move(bootstrap.native);
    evidence.slotWindows.assign(profile.slotCount, DirectUiWindowEvidence{});
    std::vector<HWND> backingWindows;
    for (const auto& candidate : bootstrap.descendants) {
        if (!candidate.window.visible || candidate.window.hwnd == evidence.directUi.hwnd) continue;
        const bool admittedClass = std::any_of(
            profile.slots, profile.slots + profile.slotCount,
            [&](const DirectUiSlot& slot) {
                return !slot.virtualSource &&
                    FluentShell::EqualsIgnoreCase(candidate.className, slot.nativeClass);
            });
        if (admittedClass) backingWindows.push_back(candidate.window.hwnd);
    }
    std::sort(backingWindows.begin(), backingWindows.end(), [](HWND left, HWND right) {
        RECT leftRect{};
        RECT rightRect{};
        if (GetWindowRect(left, &leftRect) && GetWindowRect(right, &rightRect)) {
            if (leftRect.top != rightRect.top) return leftRect.top < rightRect.top;
            if (leftRect.left != rightRect.left) return leftRect.left < rightRect.left;
        }
        return reinterpret_cast<uintptr_t>(left) < reinterpret_cast<uintptr_t>(right);
    });
    const size_t expectedBackings = static_cast<size_t>(std::count_if(
        profile.slots, profile.slots + profile.slotCount,
        [](const DirectUiSlot& slot) { return !slot.virtualSource; }));
    if (backingWindows.size() != expectedBackings)
        return Fail(error, L"generic A/B: visible backing HWND count changed");

    std::vector<bool> filled(profile.slotCount, false);
    for (HWND backing : backingWindows) {
        if (!IsChild(evidence.directUi.hwnd, backing))
            return Fail(error, L"generic A/B: backing HWND escaped the DirectUI anchor");
        const auto backingClass = ClassName(backing);
        DirectUiWindowEvidence value;
        const bool isButton = FluentShell::EqualsIgnoreCase(backingClass, L"Button");
        const bool isProgress = FluentShell::EqualsIgnoreCase(backingClass, PROGRESS_CLASSW);
        if (!ReadWindowEvidence(agent, backing, isButton, value, error, isProgress)) return false;
        bool matched = false;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const auto& slot = profile.slots[index];
            if (slot.virtualSource || filled[index] ||
                !FluentShell::EqualsIgnoreCase(backingClass, slot.nativeClass)) continue;
            const bool styleMatches = slot.nativeStyleMask == 0 ||
                (value.style & slot.nativeStyleMask) == slot.nativeStyleValue ||
                (value.style & slot.nativeStyleMask) == slot.nativeStyleAlt;
            const bool idMatches = slot.nativeControlId < 0 ||
                value.controlId == slot.nativeControlId;
            if (!styleMatches || !idMatches) continue;
            value.tabIndex = slot.tabIndex;
            evidence.slotWindows[index] = std::move(value);
            filled[index] = true;
            matched = true;
            break;
        }
        if (!matched)
            return Fail(error, L"generic A/B: backing HWND no longer matches a semantic slot"
                L" (hwnd=" + std::to_wstring(reinterpret_cast<uintptr_t>(backing)) +
                L", class=" + backingClass + L", style=" + std::to_wstring(value.style) +
                L", id=" + std::to_wstring(value.controlId) + L")");
    }
    for (size_t index = 0; index < profile.slotCount; ++index) {
        if (!profile.slots[index].virtualSource && !filled[index])
            return Fail(error, L"generic A/B: semantic backing slot is unavailable");
    }

    if (profile.directUiOwnsTabOrder) {
        const HWND first = GetNextDlgTabItem(agent.Root(), nullptr, FALSE);
        if (first != evidence.directUi.hwnd || GetNextDlgTabItem(agent.Root(), first, FALSE) != first)
            return Fail(error, L"generic A/B: DirectUI anchor is not the native tab cycle");
    }
    evidence.mutationEpoch = agent.MutationEpoch();
    evidence.lastMutationHwnd = agent.LastMutationHwnd();
    evidence.lastMutationMessage = agent.LastMutationMessage();
    return true;
}

bool MatchBootstrapToGenerated(
    const DirectUiBootstrapEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) {
    constexpr std::wstring_view prefix =
        L"generic B: native evidence changed during semantic discovery: ";
    if (before.native.mutationEpoch != after.mutationEpoch)
        return Fail(error, std::wstring(prefix) + L"mutation epoch " +
            std::to_wstring(before.native.mutationEpoch) + L" -> " +
            std::to_wstring(after.mutationEpoch) + L", last hwnd=" +
            std::to_wstring(reinterpret_cast<uintptr_t>(after.lastMutationHwnd)) +
            L", message=" + std::to_wstring(after.lastMutationMessage));
    if (!SameWindow(before.native.root, after.root))
        return Fail(error, std::wstring(prefix) + L"root facets");
    if (!SameWindow(before.native.directUi, after.directUi))
        return Fail(error, std::wstring(prefix) + L"DirectUI anchor facets");
    if (before.native.dpi != after.dpi || before.native.cloaked != after.cloaked ||
        before.native.ownerHwnd != after.ownerHwnd || before.native.title != after.title)
        return Fail(error, std::wstring(prefix) + L"top-level identity");
    if (!SameRect(before.native.clientBounds, after.clientBounds) ||
        before.native.clientOriginScreen.x != after.clientOriginScreen.x ||
        before.native.clientOriginScreen.y != after.clientOriginScreen.y)
        return Fail(error, std::wstring(prefix) + L"client geometry");
    if (before.native.pageHosts != after.pageHosts ||
        before.native.pageStatics != after.pageStatics)
        return Fail(error, std::wstring(prefix) + L"implementation HWND identity");
    return true;
}

} // namespace

const DirectUiWindowProfile* ResolveDirectUiWindowProfile(
    std::wstring& imagePath,
    std::wstring& error) {
    imagePath.clear();
    error.clear();
    wchar_t image[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
    if (!length || length >= std::size(image)) return nullptr;
    const std::wstring_view base = FluentShell::FileNameOf(
        std::wstring_view(image, length));
    const DirectUiWindowProfile* candidate = nullptr;
    for (size_t index = 0; index < kDirectUiProfileCount; ++index) {
        if (FluentShell::EqualsIgnoreCase(base, kDirectUiProfiles[index].executableBasename)) {
            candidate = &kDirectUiProfiles[index];
            break;
        }
    }
    if (!candidate) return nullptr;
    wchar_t systemDirectory[32768]{};
    const UINT systemLength =
        GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (!systemLength || systemLength >= std::size(systemDirectory))
        return Fail(error, L"process: System32 path unavailable") ? nullptr : nullptr;
    std::wstring expected(systemDirectory, systemLength);
    expected += L"\\";
    expected += candidate->executableBasename;
    imagePath = CanonicalPath(std::wstring_view(image, length));
    const auto expectedCanonical = CanonicalPath(expected);
    if (imagePath.empty() || expectedCanonical.empty() ||
        !FluentShell::EqualsIgnoreCase(imagePath, expectedCanonical)) {
        Fail(error, L"process: image is not the canonical System32 executable");
        return nullptr;
    }
    if (!ExactFileVersion(imagePath, *candidate)) {
        Fail(error, L"process: file version does not match the admitted profile");
        return nullptr;
    }
    if (!VerifyMicrosoftSignature(imagePath, error)) return nullptr;
    return candidate;
}

bool ResolveGenericDirectUiImage(std::wstring& imagePath, std::wstring& error) {
    imagePath.clear();
    error.clear();
    wchar_t image[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, image, static_cast<DWORD>(std::size(image)));
    if (!length || length >= std::size(image))
        return Fail(error, L"generic process: image path unavailable");
    const auto base = FluentShell::FileNameOf(std::wstring_view(image, length));
    if (base.empty()) return Fail(error, L"generic process: image basename unavailable");

    wchar_t systemDirectory[32768]{};
    const UINT systemLength =
        GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
    if (!systemLength || systemLength >= std::size(systemDirectory))
        return Fail(error, L"generic process: System32 path unavailable");
    std::wstring expected(systemDirectory, systemLength);
    expected.push_back(L'\\');
    expected.append(base);
    imagePath = CanonicalPath(std::wstring_view(image, length));
    const auto expectedCanonical = CanonicalPath(expected);
    if (imagePath.empty() || expectedCanonical.empty() ||
        !FluentShell::EqualsIgnoreCase(imagePath, expectedCanonical)) {
        return Fail(error, L"generic process: image is not the canonical System32 executable");
    }
    return VerifyMicrosoftSignature(imagePath, error);
}

bool CaptureDirectUiBootstrapEvidenceOnSourceThread(
    SourceThreadAgent& agent,
    DirectUiBootstrapEvidence& evidence,
    std::wstring& error) noexcept {
    try {
        return CaptureDirectUiBootstrapCore(agent, evidence, error);
    } catch (...) {
        return Fail(error, L"bootstrap: native evidence exception");
    }
}

bool CaptureDirectUiNativeEvidenceOnSourceThread(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DirectUiNativeEvidence& evidence,
    std::wstring& error) noexcept {
    try {
        if (profile.genericSemantic)
            return CaptureGeneratedProfileEvidence(agent, profile, evidence, error);
        const HWND root = agent.Root();
        if (!FluentShell::EqualsIgnoreCase(ClassName(root), profile.rootClass))
            return Fail(error, L"A/B: root class does not match the profile");
        DWORD rootProcess = 0;
        const DWORD rootThread = GetWindowThreadProcessId(root, &rootProcess);
        if (!rootThread || rootThread != agent.ThreadId() || rootProcess != GetCurrentProcessId())
            return Fail(error, L"A/B: root thread or process identity mismatch");
        evidence = {};
        evidence.mutationEpoch = agent.MutationEpoch();
        DWORD cloakReasons = 0;
        if (FAILED(DwmGetWindowAttribute(root, DWMWA_CLOAKED,
                &cloakReasons, sizeof(cloakReasons))))
            return Fail(error, L"A/B: native cloak state is unavailable");
        evidence.cloaked = (cloakReasons & DWM_CLOAKED_APP) != 0;
        evidence.dpi = GetDpiForWindow(root);
        if (!evidence.dpi || !ReadWindowEvidence(agent, root, false, evidence.root, error))
            return false;
        evidence.ownerHwnd = GetWindow(root, GW_OWNER);
        evidence.title = evidence.root.text;
        if (!GetClientRect(root, &evidence.clientBounds))
            return Fail(error, L"A/B: root client geometry unavailable");
        evidence.clientOriginScreen = { 0, 0 };
        if (!ClientToScreen(root, &evidence.clientOriginScreen))
            return Fail(error, L"A/B: root client origin unavailable");

        std::vector<HWND> descendants;
        EnumChildWindows(root, [](HWND window, LPARAM raw) -> BOOL {
            auto& values = *reinterpret_cast<std::vector<HWND>*>(raw);
            values.push_back(window);
            return values.size() <= 64;
        }, reinterpret_cast<LPARAM>(&descendants));
        if (descendants.empty() || descendants.size() < profile.minDescendants ||
            descendants.size() > profile.maxDescendants)
            return Fail(error, L"A/B: descendant count is outside the profile bound");

        // Census of implementation HWNDs. Every class is either part of the
        // declared census or rejects the surface; nothing is skipped silently.
        size_t visibleWrappers = 0;
        size_t hiddenWrappers = 0;
        size_t hiddenScrollBars = 0;
        size_t hiddenLinks = 0;
        size_t hiddenButtons = 0;
        size_t hiddenPageHosts = 0;
        size_t hiddenPageStaticTexts = 0;
        size_t visiblePageHosts = 0;
        size_t pageHostStaticTexts = 0;
        std::vector<HWND> backingWindows;
        evidence.slotWindows.assign(profile.slotCount, DirectUiWindowEvidence{});
        std::vector<bool> slotFilled(profile.slotCount, false);
        for (HWND child : descendants) {
            DWORD process = 0;
            if (GetWindowThreadProcessId(child, &process) != rootThread ||
                process != rootProcess)
                return Fail(error, L"A/B: descendant escaped the source thread/process");
            const auto name = ClassName(child);
            const bool visible = IsWindowVisible(child) != FALSE;
            if (FluentShell::EqualsIgnoreCase(name, L"DirectUIHWND")) {
                if (!visible || GetParent(child) != root || evidence.directUi.hwnd ||
                    !ReadWindowEvidence(agent, child, false, evidence.directUi, error))
                    return Fail(error, L"A/B: DirectUI anchor shape mismatch");
            } else if (FluentShell::EqualsIgnoreCase(name, L"CtrlNotifySink")) {
                visible ? ++visibleWrappers : ++hiddenWrappers;
            } else if (FluentShell::EqualsIgnoreCase(name, L"ScrollBar")) {
                if (visible) return Fail(error, L"A/B: visible ScrollBar outside the census");
                ++hiddenScrollBars;
            } else if (FluentShell::EqualsIgnoreCase(name, L"SysLink")) {
                if (visible) return Fail(error, L"A/B: visible SysLink outside the census");
                ++hiddenLinks;
            } else if (FluentShell::EqualsIgnoreCase(name, L"Button")) {
                if (!visible) {
                    ++hiddenButtons;
                    continue;
                }
                backingWindows.push_back(child);
            } else if (FluentShell::EqualsIgnoreCase(name, L"#32770")) {
                if (visible) ++visiblePageHosts; else ++hiddenPageHosts;
                if (visible) {
                    evidence.pageHosts.push_back(child);
                    backingWindows.push_back(child);
                }
            } else if (FluentShell::EqualsIgnoreCase(name, L"Static")) {
                const HWND parent = GetParent(child);
                const auto parentClass = parent ? ClassName(parent) : std::wstring();
                if (visible && FluentShell::EqualsIgnoreCase(parentClass, L"#32770")) {
                    ++pageHostStaticTexts;
                    evidence.pageStatics.push_back(child);
                    backingWindows.push_back(child);
                } else if (visible) {
                    // A visible Static must belong to an admitted page host.
                    bool inPageHost = false;
                    for (HWND host : evidence.pageHosts) {
                        if (GetParent(child) == host) { inPageHost = true; break; }
                    }
                    if (!inPageHost)
                        return Fail(error, L"A/B: visible Static outside a page host");
                    ++pageHostStaticTexts;
                    evidence.pageStatics.push_back(child);
                    backingWindows.push_back(child);
                } else {
                    ++hiddenPageStaticTexts;
                }
            } else {
                return Fail(error, L"A/B: unexpected visible or implementation HWND class");
            }
        }
        const DirectUiCensus& census = profile.census;
        if (!evidence.directUi.hwnd ||
            visibleWrappers != census.visibleNotifyWrappers ||
            hiddenWrappers != census.hiddenNotifyWrappers ||
            hiddenScrollBars != census.hiddenScrollBars ||
            hiddenLinks != census.hiddenSysLinks ||
            hiddenButtons != census.hiddenButtons ||
            hiddenPageHosts != census.hiddenPageHosts ||
            hiddenPageStaticTexts != census.hiddenPageStaticTexts ||
            visiblePageHosts != census.visiblePageHosts ||
            pageHostStaticTexts != census.pageHostStaticTexts) {
            error = L"A/B: implementation HWND census mismatch (visibleWrappers=" +
                std::to_wstring(visibleWrappers) + L", hiddenWrappers=" +
                std::to_wstring(hiddenWrappers) + L", hiddenScrollBars=" +
                std::to_wstring(hiddenScrollBars) + L", hiddenLinks=" +
                std::to_wstring(hiddenLinks) + L", hiddenButtons=" +
                std::to_wstring(hiddenButtons) + L", hiddenPageHosts=" +
                std::to_wstring(hiddenPageHosts) + L", hiddenPageStaticTexts=" +
                std::to_wstring(hiddenPageStaticTexts) + L", visiblePageHosts=" +
                std::to_wstring(visiblePageHosts) + L", pageHostStaticTexts=" +
                std::to_wstring(pageHostStaticTexts) + L")";
            return false;
        }

        // Bind every visible native backing HWND to a profile slot by class,
        // masked style, and control id. Geometry order makes otherwise
        // identical controls deterministic without application branches.
        std::sort(backingWindows.begin(), backingWindows.end(), [](HWND left, HWND right) {
            RECT leftRect{};
            RECT rightRect{};
            if (GetWindowRect(left, &leftRect) && GetWindowRect(right, &rightRect)) {
                if (leftRect.top != rightRect.top) return leftRect.top < rightRect.top;
                if (leftRect.left != rightRect.left) return leftRect.left < rightRect.left;
            }
            return reinterpret_cast<uintptr_t>(left) < reinterpret_cast<uintptr_t>(right);
        });
        size_t backingSlots = 0;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            if (!profile.slots[index].virtualSource) ++backingSlots;
        }
        if (backingWindows.size() != backingSlots)
            return Fail(error, L"A/B: visible native backing count does not match profile slots");
        for (HWND backingWindow : backingWindows) {
            if (!IsChild(evidence.directUi.hwnd, backingWindow))
                return Fail(error, L"A/B: native backing is not below the DirectUI anchor");
            const std::wstring backingClass = ClassName(backingWindow);
            const bool isButton = FluentShell::EqualsIgnoreCase(backingClass, L"Button");
            DirectUiWindowEvidence value;
            if (!ReadWindowEvidence(agent, backingWindow, isButton, value, error)) return false;
            bool matched = false;
            for (size_t index = 0; index < profile.slotCount; ++index) {
                const DirectUiSlot& slot = profile.slots[index];
                if (slot.virtualSource || slotFilled[index]) continue;
                if (!FluentShell::EqualsIgnoreCase(backingClass, slot.nativeClass)) continue;
                const uint64_t maskedStyle = value.style & slot.nativeStyleMask;
                const bool styleOk = slot.nativeStyleMask == 0 ||
                    maskedStyle == slot.nativeStyleValue ||
                    maskedStyle == slot.nativeStyleAlt;
                const bool idOk = slot.nativeControlId < 0 ||
                    value.controlId == slot.nativeControlId;
                if (styleOk && idOk) {
                    value.tabIndex = -1;
                    evidence.slotWindows[index] = std::move(value);
                    slotFilled[index] = true;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                error = L"A/B: native backing class/style/control id is outside the profile (class=" +
                    backingClass + L", id=" + std::to_wstring(value.controlId) +
                    L", style=" + std::to_wstring(value.style) + L")";
                return false;
            }
        }
        for (size_t index = 0; index < profile.slotCount; ++index) {
            if (!profile.slots[index].virtualSource && !slotFilled[index])
                return Fail(error, L"A/B: profile backing slot has no native window");
        }

        // DirectUI dialogs expose their host as the native dialog-manager tab
        // stop; the provider owns focus traversal inside it. Profiles pin that
        // internal order and UIA corroborates focusability at U.
        if (profile.directUiOwnsTabOrder) {
            const HWND first = GetNextDlgTabItem(root, nullptr, FALSE);
            if (first != evidence.directUi.hwnd ||
                GetNextDlgTabItem(root, first, FALSE) != first)
                return Fail(error, L"A/B: DirectUI host is not the exact native dialog tab cycle");
        } else {
            HWND current = nullptr;
            int index = 0;
            std::unordered_set<HWND> seen;
            for (;;) {
                current = GetNextDlgTabItem(root, current, FALSE);
                if (!current || !seen.insert(current).second) break;
                for (size_t slot = 0; slot < profile.slotCount; ++slot) {
                    if (evidence.slotWindows[slot].hwnd == current)
                        evidence.slotWindows[slot].tabIndex = index++;
                }
            }
        }
        // Declared traversal indexes must be complete and unique for enabled,
        // focusable slots owned by the DirectUI provider.
        std::unordered_set<int> declaredTabIndexes;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const DirectUiSlot& slot = profile.slots[index];
            if (profile.directUiOwnsTabOrder && slot.project && slot.uiaEnabled &&
                slot.uiaFocusable && slot.tabIndex < 0)
                return Fail(error, L"A/B: focusable DirectUI slot has no declared tab index");
            if (slot.project && slot.tabIndex >= 0) {
                if (slot.virtualSource && !slot.uiaFocusable)
                    return Fail(error, L"A/B: profile declares a tab order for a non-focusable slot");
                if (!declaredTabIndexes.insert(slot.tabIndex).second)
                    return Fail(error, L"A/B: profile declares duplicate tab indexes");
                if (profile.directUiOwnsTabOrder && !slot.virtualSource)
                    evidence.slotWindows[index].tabIndex = slot.tabIndex;
            }
        }
        evidence.mutationEpoch = agent.MutationEpoch();
        return true;
    } catch (...) {
        return Fail(error, L"A/B: native evidence exception");
    }
}

bool MatchDirectUiMutationBracket(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error,
    bool requireStableEpoch) noexcept {
    if (requireStableEpoch && before.mutationEpoch != after.mutationEpoch)
        return Fail(error, L"B: mutation epoch changed during UIA capture");
    if (before.dpi != after.dpi || !SameWindow(before.root, after.root) ||
        !SameWindow(before.directUi, after.directUi) ||
        before.ownerHwnd != after.ownerHwnd || before.title != after.title ||
        !SameRect(before.clientBounds, after.clientBounds) ||
        before.clientOriginScreen.x != after.clientOriginScreen.x ||
        before.clientOriginScreen.y != after.clientOriginScreen.y)
        return Fail(error, L"B: root/anchor identity or native facets changed");
    for (size_t index = 0; index < profile.slotCount; ++index) {
        if (profile.slots[index].virtualSource) continue;
        if (!SameProfileWindow(profile.slots[index],
                before.slotWindows[index], after.slotWindows[index])) {
            const auto& oldValue = before.slotWindows[index];
            const auto& newValue = after.slotWindows[index];
            return Fail(error, L"B: backing slot identity or native facets changed (slot=" +
                std::to_wstring(index) + L", kind=" +
                std::wstring(ControlKindName(profile.slots[index].kind)) + L", hwnd=" +
                std::to_wstring(reinterpret_cast<uintptr_t>(oldValue.hwnd)) + L"->" +
                std::to_wstring(reinterpret_cast<uintptr_t>(newValue.hwnd)) + L", generation=" +
                std::to_wstring(oldValue.generation) + L"->" +
                std::to_wstring(newValue.generation) + L", style=" +
                std::to_wstring(oldValue.style) + L"->" + std::to_wstring(newValue.style) +
                L", enabled=" + std::to_wstring(oldValue.enabled) + L"->" +
                std::to_wstring(newValue.enabled) + L", tab=" +
                std::to_wstring(oldValue.tabIndex) + L"->" +
                std::to_wstring(newValue.tabIndex) + L", position=" +
                std::to_wstring(oldValue.position) + L"->" +
                std::to_wstring(newValue.position) + L", indeterminate=" +
                std::to_wstring(oldValue.indeterminate) + L"->" +
                std::to_wstring(newValue.indeterminate) + L")");
        }
    }
    return true;
}

bool MatchDirectUiMoveTransition(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& before,
    const DirectUiNativeEvidence& after,
    std::wstring& error) noexcept {
    if (before.slotWindows.size() != profile.slotCount ||
        after.slotWindows.size() != profile.slotCount) {
        return Fail(error, L"move: DirectUI backing-slot evidence is incomplete");
    }
    const int64_t deltaX = static_cast<int64_t>(after.root.bounds.left) -
        before.root.bounds.left;
    const int64_t deltaY = static_cast<int64_t>(after.root.bounds.top) -
        before.root.bounds.top;
    if (!SameMovedWindow(before.root, after.root, deltaX, deltaY) ||
        !SameMovedWindow(before.directUi, after.directUi, deltaX, deltaY) ||
        before.dpi != after.dpi || before.ownerHwnd != after.ownerHwnd ||
        before.title != after.title || before.cloaked != after.cloaked ||
        !SameRect(before.clientBounds, after.clientBounds) ||
        static_cast<int64_t>(before.clientOriginScreen.x) + deltaX !=
            after.clientOriginScreen.x ||
        static_cast<int64_t>(before.clientOriginScreen.y) + deltaY !=
            after.clientOriginScreen.y ||
        before.pageHosts != after.pageHosts || before.pageStatics != after.pageStatics) {
        return Fail(error, L"move: DirectUI root or anchor changed beyond translation");
    }
    for (size_t index = 0; index < profile.slotCount; ++index) {
        if (profile.slots[index].virtualSource) continue;
        if (!SameMovedWindow(
                before.slotWindows[index], after.slotWindows[index], deltaX, deltaY)) {
            return Fail(error, L"move: DirectUI backing slot changed beyond translation");
        }
    }
    return true;
}

bool MatchDirectUiEvidence(
    const DirectUiWindowProfile& profile,
    const DirectUiNativeEvidence& native,
    const DirectUiUiaEvidence& uia,
    std::wstring& error) noexcept {
    if (uia.rootHwnd != native.root.hwnd || uia.directUiHwnd != native.directUi.hwnd)
        return Fail(error, L"U: root or DirectUI anchor backing HWND mismatch");

    // Virtual and backing slots each require exactly one UIA semantic with the
    // declared role. Backing slots additionally require geometry/label parity.
    std::unordered_set<HWND> backingSeen;
    for (size_t index = 0; index < profile.slotCount; ++index) {
        const DirectUiSlot& slot = profile.slots[index];
        if (slot.virtualSource) {
            const std::wstring_view automationId = slot.uiaAutomationId.empty()
                ? slot.semanticKey : slot.uiaAutomationId;
            const auto count = static_cast<size_t>(std::count_if(
                uia.semantics.begin(), uia.semantics.end(), [&](const auto& value) {
                    return value.semanticKey == automationId;
                }));
            const auto* semantic = FindSemanticByAutomationId(uia, automationId);
            if (!semantic || count != 1) {
                error = L"U: virtual slot " + std::wstring(slot.semanticKey) +
                    L" requires exactly one semantic (count=" + std::to_wstring(count) +
                    L", nativeCloaked=" + std::to_wstring(native.cloaked) + L")";
                return false;
            }
            const RECT& scope = slot.uiaBoundsScope == DirectUiBoundsScope::Root
                ? native.root.bounds : native.directUi.bounds;
            if ((slot.uiaControlType && semantic->controlType != slot.uiaControlType) ||
                (!slot.uiaClassName.empty() && semantic->className != slot.uiaClassName) ||
                (!slot.uiaFrameworkId.empty() && semantic->frameworkId != slot.uiaFrameworkId) ||
                semantic->enabled != slot.uiaEnabled ||
                semantic->focusable != slot.uiaFocusable ||
                semantic->actionable != slot.uiaActionable ||
                (slot.pinUiaPatterns &&
                    (semantic->patternMask != slot.uiaPatternMask ||
                     semantic->toggleState != slot.uiaToggleState)) ||
                (semantic->offscreen && !native.cloaked) ||
                semantic->bounds.right <= semantic->bounds.left ||
                semantic->bounds.bottom <= semantic->bounds.top ||
                !Inside(semantic->bounds, scope)) {
                error = L"U: virtual slot mismatch (slot=" + std::wstring(slot.semanticKey) +
                    L", id=" + semantic->semanticKey + L", class=" + semantic->className +
                    L", framework=" + semantic->frameworkId + L", type=" +
                    std::to_wstring(semantic->controlType) + L", focusable=" +
                    std::to_wstring(semantic->focusable) + L", enabled=" +
                    std::to_wstring(semantic->enabled) + L", actionable=" +
                    std::to_wstring(semantic->actionable) + L")";
                return false;
            }
            continue;
        }
        const DirectUiWindowEvidence& backing = native.slotWindows[index];
        const auto count = static_cast<size_t>(std::count_if(
            uia.semantics.begin(), uia.semantics.end(), [&](const auto& value) {
                return value.backingHwnd == backing.hwnd;
            }));
        const auto* byBacking = FindSemanticByBacking(uia, backing.hwnd);
        if (!byBacking || count != 1) {
            error = L"U: backing slot " + std::wstring(slot.semanticKey) +
                L" requires exactly one semantic (count=" + std::to_wstring(count) + L")";
            return false;
        }
        const bool exactSemanticClass =
            (!slot.uiaControlType || byBacking->controlType == slot.uiaControlType) &&
            (slot.uiaFrameworkId.empty() || byBacking->frameworkId == slot.uiaFrameworkId) &&
            (slot.uiaClassName.empty() ||
                byBacking->className == slot.uiaClassName);
        const bool automationIdMatches = slot.uiaAutomationId.empty() ||
            byBacking->semanticKey == slot.uiaAutomationId;
        const bool expectedBacking = backingSeen.insert(backing.hwnd).second;
        const bool geometryMatches = RectNear(byBacking->bounds, backing.bounds);
        const bool nativeLabelMatches = slot.kind == ControlKind::ProgressBar ||
            DisplayText(backing.text) == byBacking->name;
        const bool nativeNoteMatches = backing.note.empty() || backing.note == byBacking->helpText;
        if (!expectedBacking || byBacking->focusable != slot.uiaFocusable ||
            byBacking->enabled != slot.uiaEnabled ||
            byBacking->actionable != slot.uiaActionable ||
            (slot.pinUiaPatterns &&
                (byBacking->patternMask != slot.uiaPatternMask ||
                 byBacking->toggleState != slot.uiaToggleState)) ||
            (slot.project && byBacking->name.empty()) ||
            (byBacking->offscreen && !native.cloaked) ||
            !exactSemanticClass || !automationIdMatches || !geometryMatches ||
            !nativeLabelMatches || !nativeNoteMatches) {
            error = L"U: backing semantic mismatch (slot=" + std::wstring(slot.semanticKey) +
                L", key=" + byBacking->semanticKey +
                L", name=" + byBacking->name + L", class=" + byBacking->className +
                L", framework=" + byBacking->frameworkId + L", type=" +
                std::to_wstring(byBacking->controlType) + L", focusable=" +
                std::to_wstring(byBacking->focusable) + L", enabled=" +
                std::to_wstring(byBacking->enabled) + L", actionable=" +
                std::to_wstring(byBacking->actionable) + L", hwnd=" +
                std::to_wstring(reinterpret_cast<uintptr_t>(byBacking->backingHwnd)) +
                L", unique=" + std::to_wstring(expectedBacking) +
                L", semanticClass=" + std::to_wstring(exactSemanticClass) +
                L", automationId=" + std::to_wstring(automationIdMatches) +
                L", geometry=" + std::to_wstring(geometryMatches) +
                L", label=" + std::to_wstring(nativeLabelMatches) +
                L", note=" + std::to_wstring(nativeNoteMatches) + L")";
            return false;
        }
    }

    // Nothing actionable/focusable may exist beyond the declared slots. The
    // DirectUI host bounds distinguish page content from root nonclient chrome.
    size_t accounted = 0;
    for (const auto& value : uia.semantics) {
        bool declared = false;
        for (size_t index = 0; index < profile.slotCount; ++index) {
            const DirectUiSlot& slot = profile.slots[index];
            if (slot.virtualSource) {
                const std::wstring_view automationId = slot.uiaAutomationId.empty()
                    ? slot.semanticKey : slot.uiaAutomationId;
                if (value.semanticKey == automationId) { declared = true; break; }
            } else if (value.backingHwnd == native.slotWindows[index].hwnd) {
                declared = true; break;
            }
        }
        if (declared) ++accounted;
        else {
            error = L"U: unexpected actionable semantic (key=" + value.semanticKey +
                L", name=" + value.name + L", class=" + value.className +
                L", framework=" + value.frameworkId + L", type=" +
                std::to_wstring(value.controlType) + L", focusable=" +
                std::to_wstring(value.focusable) + L", actionable=" +
                std::to_wstring(value.actionable) + L", hwnd=" +
                std::to_wstring(reinterpret_cast<uintptr_t>(value.backingHwnd)) + L")";
            return false;
        }
    }
    if (accounted != uia.semantics.size())
        return Fail(error, L"U: semantic accounting mismatch");
    return true;
}

uint64_t DirectUiSemanticNodeId(
    std::wstring_view adapterId,
    std::wstring_view semanticKey) noexcept {
    uint64_t hash = 1469598103934665603ull;
    const auto append = [&](std::wstring_view value) {
        for (wchar_t character : value) {
            hash ^= static_cast<uint16_t>(character);
            hash *= 1099511628211ull;
        }
        hash ^= 0xffu;
        hash *= 1099511628211ull;
    };
    append(adapterId);
    append(semanticKey);
    return hash | (1ull << 63);
}

bool NormalizeDirectUiEvidenceText(
    std::wstring_view input,
    std::wstring& output) noexcept {
    try {
        if (input.size() > 1024 || input.find(L'\0') != std::wstring_view::npos) return false;
        // Localization is evidence. Preserve code units and whitespace exactly;
        // normalization here means only bounded, unambiguous ownership.
        output.assign(input);
        return true;
    } catch (...) {
        return false;
    }
}

bool IsMicrosoftWindowsSignerName(std::wstring_view name) noexcept {
    return FluentShell::EqualsIgnoreCase(name, L"Microsoft Windows") ||
        FluentShell::EqualsIgnoreCase(name, L"Microsoft Corporation");
}

bool DirectUiHandoffMayPost(
    bool pageRevalidated,
    bool bindingGenerationMatches,
    bool proxyIsolated,
    bool nativeVisible,
    bool nativeUncloaked) noexcept {
    return pageRevalidated && bindingGenerationMatches && proxyIsolated &&
        nativeVisible && nativeUncloaked;
}

DirectUiAdmissionResult InspectDirectUiSurface(
    SourceThreadAgent& agent,
    const DirectUiWindowProfile& profile,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& diagnostic,
    WindowSnapshot* snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>* bindings,
    DirectUiNativeEvidence* nativeEvidence) noexcept {
    diagnostic.clear();
    const auto absoluteDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    std::wstring imagePath;
    std::wstring processError;
    if (!ResolveDirectUiWindowProfile(imagePath, processError) ||
        std::chrono::steady_clock::now() >= absoluteDeadline) {
        diagnostic = L"DirectUI adapter rejected at process: " +
            (processError.empty() ? L"profile resolution failed" : processError);
        return DirectUiAdmissionResult::Rejected;
    }
    if (agent.DirectUiUiaPoisoned()) {
        diagnostic = L"DirectUI adapter rejected at U: UIA worker was previously abandoned";
        return DirectUiAdmissionResult::Rejected;
    }
    struct Result final { bool ok = false; DirectUiUiaEvidence value; std::wstring error; };
    try {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto remainingMs = [&]() -> DWORD {
                const auto now = std::chrono::steady_clock::now();
                if (now >= absoluteDeadline) return 0;
                return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    absoluteDeadline - now).count());
            };
            DirectUiNativeEvidence before;
            DWORD remaining = remainingMs();
            if (!remaining || !agent.CaptureDirectUiNativeEvidence(
                    profile, before, diagnostic, remaining, cancelEvent)) {
                diagnostic = L"DirectUI adapter rejected at A: " + diagnostic;
                return DirectUiAdmissionResult::Rejected;
            }
            auto promise = std::make_shared<std::promise<Result>>();
            auto future = promise->get_future();
            const HWND root = before.root.hwnd;
            const HWND directUi = before.directUi.hwnd;
            const DWORD processId = GetCurrentProcessId();
            const bool allowDeclaredOffscreen = before.cloaked;
            std::vector<std::wstring> virtualAutomationIds;
            std::vector<HWND> backingWindows;
            for (size_t index = 0; index < profile.slotCount; ++index) {
                const DirectUiSlot& slot = profile.slots[index];
                if (slot.virtualSource) {
                    virtualAutomationIds.emplace_back(slot.uiaAutomationId.empty()
                        ? slot.semanticKey : slot.uiaAutomationId);
                } else if (before.slotWindows[index].hwnd) {
                    backingWindows.push_back(before.slotWindows[index].hwnd);
                }
            }
            std::thread worker([promise, root, directUi, processId, allowDeclaredOffscreen,
                    virtualAutomationIds = std::move(virtualAutomationIds),
                    backingWindows = std::move(backingWindows)] {
                Result result;
                try {
                    result.ok = CaptureUia(root, directUi, processId,
                        allowDeclaredOffscreen, virtualAutomationIds, backingWindows, false,
                        result.value, result.error);
                }
                catch (...) { result.error = L"UIA evidence worker exception"; }
                try { promise->set_value(std::move(result)); } catch (...) {}
            });
            remaining = (std::min)(remainingMs(), kUiaDeadlineMs);
            auto status = remaining
                ? future.wait_for(std::chrono::milliseconds(remaining))
                : std::future_status::timeout;
            if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0)
                status = std::future_status::timeout;
            if (status != std::future_status::ready) {
                agent.PoisonDirectUiUia();
                worker.detach();
                diagnostic = L"DirectUI adapter rejected at U: bounded UIA deadline expired; worker abandoned and agent poisoned";
                return DirectUiAdmissionResult::Rejected;
            }
            worker.join();
            auto uia = future.get();
            if (!uia.ok || !MatchDirectUiEvidence(profile, before, uia.value, diagnostic)) {
                diagnostic = L"DirectUI adapter rejected at U: " +
                    (diagnostic.empty() ? uia.error : diagnostic);
                return DirectUiAdmissionResult::Rejected;
            }
            DirectUiNativeEvidence after;
            remaining = remainingMs();
            const bool capturedAfter = remaining && agent.CaptureDirectUiNativeEvidence(
                profile, after, diagnostic, remaining, cancelEvent);
            if (capturedAfter &&
                MatchDirectUiMutationBracket(profile, before, after, diagnostic)) {
                if (snapshot && bindings) {
                    WindowSnapshot composite = *snapshot;
                    std::unordered_map<uint64_t, DirectUiActionBinding> capturedBindings;
                    if (!BuildCompositeSnapshot(agent, profile, imagePath, after, uia.value,
                            composite, capturedBindings, diagnostic)) {
                        diagnostic = L"DirectUI adapter rejected at composite: " + diagnostic;
                        return DirectUiAdmissionResult::Rejected;
                    }
                    *snapshot = std::move(composite);
                    *bindings = std::move(capturedBindings);
                }
                if (nativeEvidence) *nativeEvidence = std::move(after);
                diagnostic = L"DirectUI adapter admitted exact signed page";
                return DirectUiAdmissionResult::Admitted;
            }
            if (attempt == 0 &&
                diagnostic == L"B: mutation epoch changed during UIA capture")
                continue;
            diagnostic = L"DirectUI adapter rejected at B: " + diagnostic;
            return DirectUiAdmissionResult::Rejected;
        }
        diagnostic = L"DirectUI adapter rejected at B: mutation bracket retry exhausted";
        return DirectUiAdmissionResult::Rejected;
    } catch (...) {
        diagnostic = L"DirectUI adapter rejected at U: worker setup exception";
        return DirectUiAdmissionResult::Rejected;
    }
}

DirectUiAdmissionResult InspectGenericDirectUiSurface(
    SourceThreadAgent& agent,
    DWORD timeoutMs,
    HANDLE cancelEvent,
    std::wstring& diagnostic,
    std::shared_ptr<DirectUiOwnedProfile>* ownedProfile,
    WindowSnapshot* snapshot,
    std::unordered_map<uint64_t, DirectUiActionBinding>* bindings,
    DirectUiNativeEvidence* nativeEvidence) noexcept {
    diagnostic.clear();
    const auto absoluteDeadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    std::wstring imagePath;
    if (!ResolveGenericDirectUiImage(imagePath, diagnostic)) {
        diagnostic = L"Generic DirectUI adapter rejected at process: " + diagnostic;
        return DirectUiAdmissionResult::Rejected;
    }
    if (agent.DirectUiUiaPoisoned()) {
        diagnostic = L"Generic DirectUI adapter rejected at U: UIA worker was previously abandoned";
        return DirectUiAdmissionResult::Rejected;
    }
    struct Result final { bool ok = false; DirectUiUiaEvidence value; std::wstring error; };
    try {
        constexpr int kMutationBracketAttempts = 2;
        for (int attempt = 0; attempt < kMutationBracketAttempts; ++attempt) {
            const auto remainingMs = [&]() -> DWORD {
                const auto now = std::chrono::steady_clock::now();
                if (now >= absoluteDeadline) return 0;
                return static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    absoluteDeadline - now).count());
            };
            DirectUiBootstrapEvidence before;
            DWORD remaining = remainingMs();
            if (!remaining || !agent.CaptureDirectUiBootstrapEvidence(
                    before, diagnostic, remaining, cancelEvent)) {
                diagnostic = L"Generic DirectUI adapter rejected at A: " + diagnostic;
                return DirectUiAdmissionResult::Rejected;
            }

            auto promise = std::make_shared<std::promise<Result>>();
            auto future = promise->get_future();
            const HWND root = before.native.root.hwnd;
            const HWND directUi = before.native.directUi.hwnd;
            const DWORD processId = GetCurrentProcessId();
            std::thread worker([promise, root, directUi, processId] {
                Result result;
                try {
                    result.ok = CaptureUia(root, directUi, processId, false, {}, {}, true,
                        result.value, result.error);
                } catch (...) {
                    result.error = L"generic UIA evidence worker exception";
                }
                try { promise->set_value(std::move(result)); } catch (...) {}
            });
            remaining = (std::min)(remainingMs(), kUiaDeadlineMs);
            auto status = remaining
                ? future.wait_for(std::chrono::milliseconds(remaining))
                : std::future_status::timeout;
            if (cancelEvent && WaitForSingleObject(cancelEvent, 0) == WAIT_OBJECT_0)
                status = std::future_status::timeout;
            if (status != std::future_status::ready) {
                agent.PoisonDirectUiUia();
                worker.detach();
                diagnostic = L"Generic DirectUI adapter rejected at U: bounded UIA deadline expired; worker abandoned and agent poisoned";
                return DirectUiAdmissionResult::Rejected;
            }
            worker.join();
            auto uia = future.get();
            if (!uia.ok) {
                diagnostic = L"Generic DirectUI adapter rejected at U: " + uia.error;
                return DirectUiAdmissionResult::Rejected;
            }

            std::shared_ptr<DirectUiOwnedProfile> discovered;
            if (!BuildGenericSemanticProfile(
                    before, uia.value, imagePath, discovered, diagnostic)) {
                diagnostic = L"Generic DirectUI adapter rejected at contract: " + diagnostic;
                return DirectUiAdmissionResult::Rejected;
            }
            agent.AdoptDirectUiProfile(discovered);
            DirectUiNativeEvidence after;
            remaining = remainingMs();
            const bool capturedAfter = remaining && agent.CaptureDirectUiNativeEvidence(
                discovered->profile, after, diagnostic, remaining, cancelEvent);
            if (capturedAfter && MatchBootstrapToGenerated(before, after, diagnostic) &&
                MatchDirectUiEvidence(discovered->profile, after, uia.value, diagnostic)) {
                if (snapshot && bindings) {
                    WindowSnapshot composite = *snapshot;
                    std::unordered_map<uint64_t, DirectUiActionBinding> capturedBindings;
                    if (!BuildCompositeSnapshot(agent, discovered->profile, imagePath, after,
                            uia.value, composite, capturedBindings, diagnostic)) {
                        diagnostic = L"Generic DirectUI adapter rejected at composite: " + diagnostic;
                        return DirectUiAdmissionResult::Rejected;
                    }
                    *snapshot = std::move(composite);
                    *bindings = std::move(capturedBindings);
                }
                if (nativeEvidence) *nativeEvidence = std::move(after);
                if (ownedProfile) *ownedProfile = discovered;
                diagnostic = L"Generic DirectUI adapter admitted capability-derived semantic page";
                return DirectUiAdmissionResult::Admitted;
            }
            if (attempt + 1 < kMutationBracketAttempts &&
                diagnostic.starts_with(
                    L"generic B: native evidence changed during semantic discovery"))
                continue;
            diagnostic = L"Generic DirectUI adapter rejected at B: " + diagnostic;
            return DirectUiAdmissionResult::Rejected;
        }
        diagnostic = L"Generic DirectUI adapter rejected at B: mutation bracket retry exhausted";
        return DirectUiAdmissionResult::Rejected;
    } catch (...) {
        diagnostic = L"Generic DirectUI adapter rejected at U: worker setup exception";
        return DirectUiAdmissionResult::Rejected;
    }
}

} // namespace FluentShell::Bridge::Translation
