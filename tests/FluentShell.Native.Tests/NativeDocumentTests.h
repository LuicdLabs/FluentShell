#pragma once

#include "MmcHtmlDocumentTests.h"
#include "../../src/Bridge/Translation/NativeDocument.h"
#include "../../src/Bridge/Translation/GuiThreadDispatch.h"

#include <atomic>
#include <memory>
#include <filesystem>
#include <fstream>
#include <winrt/Windows.Data.Json.h>

namespace FluentShell::Tests {
namespace NativeDocumentTests {

namespace Translation = FluentShell::Bridge::Translation;
using Microsoft::WRL::ComPtr;
using MmcHtmlDocumentTests::Text;
using Translation::NativeDocumentSnapshot;

struct Fixture final {
    MmcHtmlDocumentTests::Fixture html;
    ~Fixture() { Translation::ForgetNativeDocument(html.Window()); }
    bool Capture(NativeDocumentSnapshot& snapshot, std::wstring& reason) {
        return Translation::NativeDocumentDetail::ReadContent(html.Document(), html.Window(),
            MmcHtmlDocumentTests::Fixture::kViewport, snapshot, reason);
    }
};

inline std::wstring Page(const wchar_t* extra = L"") {
    // Keep unrelated boxes inside the viewport. The sample owns a real vertical
    // scrollport, while its semantic text retains the full content rectangle.
    return std::wstring(L"<!doctype html><html><head><style>"
        L"html,body{margin:0;padding:0;width:640px;height:480px;font:12px Arial;}"
        L"h1,p,div,a,span{margin:0;padding:0;}"
        L"#Title{position:absolute;left:8px;top:8px;width:400px;height:30px;}"
        L"#Action{position:absolute;left:8px;top:46px;width:240px;height:24px;display:block;}"
        L"#Samples{position:absolute;left:8px;top:84px;width:320px;height:120px;overflow:auto;}"
        L"#Reading{width:260px;height:400px;}"
        L"#Status{position:absolute;left:8px;top:220px;width:300px;height:24px;}"
        L"#Extra{position:absolute;left:8px;top:260px;width:500px;height:60px;}"
        L"</style></head><body><h1 id='Title'>Overview</h1>"
        L"<a id='Action' href='#first'>Open</a>"
        L"<div id='Samples'><p id='Reading'>Sample 12</p></div>"
        L"<p id='Status'>idle</p>") + extra + L"</body></html>";
}

inline bool Load(Fixture& fixture, const wchar_t* extra, void (*check)(bool, const char*)) {
    return MmcHtmlDocumentTests::Load(fixture.html, Page(extra), "native document semantics", check);
}

inline bool Capture(Fixture& fixture, NativeDocumentSnapshot& snapshot,
    void (*check)(bool, const char*)) {
    std::wstring reason;
    const bool admitted = fixture.Capture(snapshot, reason);
    if (!admitted) {
        Text ready, editing;
        fixture.html.Document()->get_readyState(&ready.value);
        fixture.html.Document()->get_designMode(&editing.value);
        std::wcerr << L"Native document positive fixture rejected: " << reason <<
            L" (ready=" << ready.String() << L", designMode=" << editing.String() << L")\n";
    }
    check(admitted, "native document rejected a semantic heading, link, text, and scrollport");
    return admitted;
}

inline int FindItem(const NativeDocumentSnapshot& snapshot, const wchar_t* kind,
    const wchar_t* name = nullptr) {
    for (size_t index = 0; index < snapshot.items.size(); ++index)
        if (snapshot.items[index].kind == kind && (!name || snapshot.items[index].name == name))
            return static_cast<int>(index);
    return -1;
}

inline std::wstring InnerText(Fixture& fixture, const wchar_t* id) {
    auto element = fixture.html.Element(id);
    Text text;
    return element && SUCCEEDED(element->get_innerText(&text.value)) ? text.String() : L"";
}

inline bool SetClick(Fixture& fixture, const wchar_t* result) {
    const auto script = std::wstring(L"document.getElementById('Action').onclick=function(){"
        L"document.getElementById('Status').innerText='") + result + L"';return false;};";
    return fixture.html.Execute(script.c_str());
}

inline void CheckWire(const NativeDocumentSnapshot& captured, void (*check)(bool, const char*)) {
    constexpr auto nonce = L"00112233445566778899aabbccddeeff";
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42";
    snapshot.generation = 1;
    snapshot.revision = 7;
    Translation::ControlNode node;
    node.nodeId = 1;
    node.generation = 1;
    node.kind = Translation::ControlKind::NativeDocument;
    node.visible = node.enabled = true;
    node.document = captured;
    auto& document = *node.document;
    const int linkIndex = FindItem(document, L"link", L"Open");
    check(linkIndex == 1 && !document.regions.empty(), "native document shared fixture shape changed");
    if (linkIndex != 1 || document.regions.empty()) return;
    document.documentId = L"document-1";
    document.items[1].key = L"link";
    document.items[1].actionIdentity = L"9007199254740993";
    document.regions[0].key = L"details";
    snapshot.nodes.push_back(std::move(node));
    const auto payload = Translation::SerializeWindowOpen(nonce, snapshot);
    const auto wire = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(payload))
        .GetNamedObject(L"window").GetNamedArray(L"nodes").GetObjectAt(0).GetNamedObject(L"document");
    check(wire.GetNamedString(L"documentId") == L"document-1" &&
        wire.GetNamedArray(L"items").GetObjectAt(1).GetNamedString(L"actionIdentity") == L"9007199254740993" &&
        wire.GetNamedArray(L"regions").GetObjectAt(0).GetNamedString(L"key") == L"details",
        "native document serialization lost its lossless action or scroll identity");
    const auto read = [](const wchar_t* name) {
        std::ifstream file(std::filesystem::current_path() / L"tests" / L"ProtocolFixtures" / name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    };
    std::wstring reason;
    Translation::ActionRequest link, scroll;
    check(Translation::ParseActionInvoke(read(L"action.invoke.native-document-link.json"), nonce, link, reason) &&
        link.itemIndex == 1 && link.nativeDocumentActionIdentity == L"9007199254740993" &&
        Translation::ValidateActionForSnapshot(link, snapshot, reason),
        "shared native document link action did not retain its canonical identity");
    check(Translation::ParseActionInvoke(read(L"action.invoke.native-document-scroll.json"), nonce, scroll, reason) &&
        scroll.itemIndex == 0 && scroll.integerValues == std::vector<int>{0, 80} &&
        Translation::ValidateActionForSnapshot(scroll, snapshot, reason),
        "shared native document scroll action did not retain its absolute offset");
    link.nativeDocumentActionIdentity = L"9007199254740992";
    check(!Translation::ValidateActionForSnapshot(link, snapshot, reason),
        "native document admitted a stale link action identity");
    scroll.nativeDocumentId = L"retired-document";
    check(!Translation::ValidateActionForSnapshot(scroll, snapshot, reason),
        "native document admitted a scroll request for a retired page");
}

inline void CheckSemanticsAndIdentity(void (*check)(bool, const char*)) {
    Fixture fixture;
    if (!Load(fixture, L"", check)) return;
    check(SetClick(fixture, L"clicked"), "native document fixture could not install its link handler");
    NativeDocumentSnapshot first;
    if (!Capture(fixture, first, check)) return;
    CheckWire(first, check);
    const int linkIndex = FindItem(first, L"link", L"Open");
    const int sampleIndex = FindItem(first, L"text", L"Sample 12");
    check(!first.documentId.empty() && first.items.size() == 4 &&
        FindItem(first, L"heading", L"Overview") >= 0 && linkIndex >= 0 && sampleIndex >= 0 &&
        first.regions.size() == 1 && first.regions[0].kind == L"html" &&
        first.regions[0].maximumY > 0 && first.regions[0].offsetY == 0,
        "native document lost semantic content or its real scroll range");
    if (linkIndex < 0 || sampleIndex < 0 || first.regions.size() != 1) return;
    const auto originalLink = first.items[linkIndex];
    const auto& sample = first.items[sampleIndex];
    check(!originalLink.actionIdentity.empty() && originalLink.enabled &&
        sample.regionIndex == 0 && sample.rect.top == 0 &&
        sample.rect.bottom > first.regions[0].rect.bottom - first.regions[0].rect.top &&
        sample.rect.bottom <= first.regions[0].contentHeight,
        "native document clipped scroll content or omitted its link binding");

    check(fixture.html.Execute(L"document.getElementById('Reading').innerText='Sample 13';"),
        "native document fixture could not update a sample");
    NativeDocumentSnapshot sampled;
    if (!Capture(fixture, sampled, check)) return;
    const int sampledLink = FindItem(sampled, L"link", L"Open");
    const bool stable = sampled.documentId == first.documentId && sampledLink >= 0 &&
        FindItem(sampled, L"text", L"Sample 13") >= 0 &&
        sampled.items[sampledLink].key == originalLink.key &&
        sampled.items[sampledLink].actionIdentity == originalLink.actionIdentity;
    if (!stable && sampledLink >= 0)
        std::wcerr << L"Native document sample identity: document=" << first.documentId << L" -> " <<
            sampled.documentId << L", token=" << originalLink.actionIdentity << L" -> " <<
            sampled.items[sampledLink].actionIdentity << L'\n';
    check(stable, "native document changed an unchanged link binding when only a sample changed");
    if (sampledLink < 0) return;

    check(fixture.html.Execute(L"document.getElementById('Action').href='#second';"),
        "native document fixture could not replace a link destination");
    NativeDocumentSnapshot destination;
    if (!Capture(fixture, destination, check)) return;
    const int destinationLink = FindItem(destination, L"link", L"Open");
    check(destinationLink >= 0 && destination.items[destinationLink].key == originalLink.key &&
        destination.items[destinationLink].actionIdentity != originalLink.actionIdentity,
        "native document reused a binding after the link destination changed");
    if (destinationLink < 0) return;
    std::wstring reason;
    check(!Translation::NativeDocumentDetail::InvokeItem(fixture.html.Document(), sampled,
        sampledLink, reason) && InnerText(fixture, L"Status") == L"idle",
        "native document invoked a stale link after its destination changed");

    check(SetClick(fixture, L"replacement"), "native document fixture could not replace its link handler");
    NativeDocumentSnapshot handler;
    if (!Capture(fixture, handler, check)) return;
    const int handlerLink = FindItem(handler, L"link", L"Open");
    check(handlerLink >= 0 && handler.items[handlerLink].key == originalLink.key &&
        handler.items[handlerLink].actionIdentity != destination.items[destinationLink].actionIdentity,
        "native document reused a binding after the link handler changed");
    if (handlerLink < 0) return;
    check(!Translation::NativeDocumentDetail::InvokeItem(fixture.html.Document(), destination,
        destinationLink, reason) && InnerText(fixture, L"Status") == L"idle",
        "native document invoked a stale link after its handler changed");
    check(Translation::NativeDocumentDetail::InvokeItem(fixture.html.Document(), handler,
        handlerLink, reason) && InnerText(fixture, L"Status") == L"replacement",
        "native document did not invoke the current native link handler");
}

inline void CheckCensus(void (*check)(bool, const char*)) {
    struct Rejection {
        const wchar_t* extra;
        const wchar_t* script;
        const wchar_t* reason;
        const char* scenario;
        const wchar_t* alternativeReason = nullptr;
    };
    const Rejection cases[] = {
        {L"<div id='Extra'>Unrepresented <span>child text</span></div>", nullptr,
            L"text outside its represented semantics", "native document dropped mixed parent text"},
        {L"<div id='Extra'><input value='Editable'></div>", nullptr,
            L"unrepresented element", "native document silently discarded an input control",
            L"element style or editing semantics"},
        {L"<div id='Extra' tabindex='0'>Focusable text</div>", nullptr,
            L"focus target", "native document discarded a native keyboard focus target"},
        {L"", L"document.oncontextmenu=function(){return false;};",
            L"document input handler", "native document discarded a document context-menu handler"},
    };
    for (const auto& test : cases) {
        Fixture fixture;
        if (!Load(fixture, test.extra, check)) return;
        if (test.script)
            check(fixture.html.Execute(test.script), "native document fixture could not install an input handler");
        NativeDocumentSnapshot snapshot;
        std::wstring reason;
        const bool admitted = fixture.Capture(snapshot, reason);
        const bool expectedReason = reason.find(test.reason) != std::wstring::npos ||
            (test.alternativeReason && reason.find(test.alternativeReason) != std::wstring::npos);
        if (admitted || !expectedReason)
            std::wcerr << L"Native document unexpected admission/reason: " << reason << L'\n';
        check(!admitted && expectedReason, test.scenario);
    }
}

inline void CheckCancellationAndRetirement(void (*check)(bool, const char*)) {
    Fixture fixture;
    if (!Load(fixture, L"", check)) return;
    check(SetClick(fixture, L"clicked"), "native document fixture could not install its cancellable link handler");
    NativeDocumentSnapshot snapshot;
    if (!Capture(fixture, snapshot, check)) return;
    const int link = FindItem(snapshot, L"link", L"Open");
    auto element = fixture.html.Element(L"Samples");
    ComPtr<IHTMLElement2> scrollport;
    const bool scrollable = element && SUCCEEDED(element.As(&scrollport)) &&
        snapshot.regions.size() == 1 && snapshot.regions[0].maximumY >= 30;
    check(link >= 0 && scrollable, "native document action fixture has no usable native actions");
    if (link < 0 || !scrollable) return;
    std::wstring reason;
    auto cancelled = std::make_shared<std::atomic<bool>>(true);
    {
        Translation::GuiThreadOperationScope scope(cancelled, GetTickCount64() + 10000);
        const bool invoked = Translation::NativeDocumentDetail::InvokeItem(
            fixture.html.Document(), snapshot, link, reason);
        if (invoked || reason.find(L"cancelled") == std::wstring::npos)
            std::wcerr << L"Native document cancelled invocation: " << reason << L'\n';
        check(!invoked && reason.find(L"cancelled") != std::wstring::npos,
            "native document admitted an already-cancelled link invocation");
        const bool scrolled = Translation::NativeDocumentDetail::Scroll(
            fixture.html.Document(), snapshot, 0, 0, 30, reason);
        if (scrolled || reason.find(L"cancelled") == std::wstring::npos)
            std::wcerr << L"Native document cancelled scroll: " << reason << L'\n';
        check(!scrolled && reason.find(L"cancelled") != std::wstring::npos,
            "native document admitted an already-cancelled scroll mutation");
    }
    long y = -1;
    check(InnerText(fixture, L"Status") == L"idle" &&
        SUCCEEDED(scrollport->get_scrollTop(&y)) && y == 0,
        "cancelled native document actions changed the source document");
    check(Translation::NativeDocumentDetail::InvokeItem(fixture.html.Document(), snapshot, link, reason) &&
        InnerText(fixture, L"Status") == L"clicked",
        "native document cancellation test never reached a usable link action");
    check(Translation::NativeDocumentDetail::Scroll(fixture.html.Document(), snapshot, 0, 0, 30, reason) &&
        SUCCEEDED(scrollport->get_scrollTop(&y)) && y == 30,
        "native document cancellation test never reached a usable scroll action");
    check(Translation::NativeDocumentDetail::Scroll(fixture.html.Document(), snapshot, 0, 0, 0, reason) &&
        fixture.html.Execute(L"document.getElementById('Status').innerText='idle';"),
        "native document fixture could not reset its native actions");
    NativeDocumentSnapshot current;
    if (!Capture(fixture, current, check)) return;
    const int currentLink = FindItem(current, L"link", L"Open");
    if (currentLink < 0) { check(false, "native document lost its link before retirement"); return; }
    Translation::ForgetNativeDocument(fixture.html.Window());
    check(!Translation::NativeDocumentDetail::InvokeItem(fixture.html.Document(), current, currentLink, reason) &&
        (reason.find(L"expired") != std::wstring::npos || reason.find(L"identity") != std::wstring::npos),
        "native document retained an action binding after forgetting its host");
    check(!Translation::NativeDocumentDetail::Scroll(fixture.html.Document(), current, 0, 0, 30, reason) &&
        (reason.find(L"expired") != std::wstring::npos || reason.find(L"identity") != std::wstring::npos),
        "native document retained a scroll binding after forgetting its host");
    check(InnerText(fixture, L"Status") == L"idle" &&
        SUCCEEDED(scrollport->get_scrollTop(&y)) && y == 0,
        "retired native document bindings mutated the source document");
    NativeDocumentSnapshot reopened;
    if (!Capture(fixture, reopened, check)) return;
    const int reopenedLink = FindItem(reopened, L"link", L"Open");
    check(reopened.documentId != current.documentId && reopenedLink >= 0 &&
        reopened.items[reopenedLink].actionIdentity != current.items[currentLink].actionIdentity,
        "native document reused retired document or action identity after recapture");
}

inline void Run(void (*check)(bool, const char*)) {
    CheckSemanticsAndIdentity(check);
    CheckCensus(check);
    CheckCancellationAndRetirement(check);
}

} // namespace NativeDocumentTests

inline void TestNativeDocumentAdmission(void (*check)(bool, const char*)) {
    std::thread source([check] {
        const HRESULT initialized = OleInitialize(nullptr);
        check(SUCCEEDED(initialized), "native document test STA could not initialize OLE");
        if (FAILED(initialized)) return;
        try { NativeDocumentTests::Run(check); }
        catch (...) { check(false, "native document test fixture raised an exception"); }
        OleUninitialize();
    });
    source.join();
}

} // namespace FluentShell::Tests
