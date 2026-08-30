#include "../../src/Bridge/Ipc/FrameCodec.h"
#include "../../src/Bridge/Translation/WindowSnapshot.h"
#include "../../src/Bridge/Translation/ControlAdapters.h"
#include "../../src/Bridge/Translation/WindowCapture.h"

#include <winrt/base.h>
#include <objbase.h>
#include <commctrl.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace FluentShell::Bridge;

int g_failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL: " << message << '\n';
}

std::string ReadFixture(const wchar_t* name) {
    const auto path = std::filesystem::current_path() / L"tests" / L"ProtocolFixtures" / name;
    std::ifstream stream(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>() };
}

std::string ReplaceOnce(std::string value, std::string_view from, std::string_view to) {
    const auto position = value.find(from);
    if (position != std::string::npos) value.replace(position, from.size(), to);
    return value;
}

void TestHeaderValidation() {
    Check(sizeof(Ipc::FrameHeader) == 32, "FLSH header must remain 32 bytes");
    Ipc::FrameHeader header{};
    header.type = static_cast<uint16_t>(Ipc::MessageType::Heartbeat);
    header.sequence = 1;
    std::wstring error;
    Check(Ipc::ValidateHeader(header, 0, error), "valid header was rejected");

    auto invalid = header;
    invalid.major = 2;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "major mismatch was accepted");
    invalid = header;
    invalid.flags = 1;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "unknown flags were accepted");
    invalid = header;
    invalid.type = 99;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "unknown message type was accepted");
    invalid = header;
    invalid.payloadLength = Ipc::kMaxPayloadBytes + 1;
    Check(!Ipc::ValidateHeader(invalid, 0, error), "oversized payload was accepted");
    invalid = header;
    invalid.minor = static_cast<uint16_t>(Ipc::kProtocolMinor + 1);
    Check(Ipc::ValidateHeader(invalid, 0, error),
        "a newer same-major protocol minor was rejected");
    Check(!Ipc::ValidateHeader(header, 1, error), "duplicate sequence was accepted");
}

void TestPipeRoundTrip() {
    const std::wstring name = L"\\\\.\\pipe\\FluentShell.NativeTests." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE server = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    Check(server != INVALID_HANDLE_VALUE, "test named pipe server creation failed");
    if (server == INVALID_HANDLE_VALUE) return;
    HANDLE client = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(client != INVALID_HANDLE_VALUE, "test named pipe client creation failed");
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    const BOOL connected = ConnectNamedPipe(server, nullptr);
    Check(connected || GetLastError() == ERROR_PIPE_CONNECTED, "test pipe did not connect");

    std::wstring error;
    const std::string payload = "{\"ok\":true}";
    Check(Ipc::WriteFrame(server, Ipc::MessageType::Heartbeat, 1, 7, payload, error),
        "native WriteFrame failed");
    std::vector<unsigned char> wire(sizeof(Ipc::FrameHeader) + payload.size());
    DWORD read = 0;
    Check(ReadFile(client, wire.data(), static_cast<DWORD>(wire.size()), &read, nullptr) &&
        read == wire.size(), "could not read encoded frame bytes");
    Check(wire[0] == 'F' && wire[1] == 'L' && wire[2] == 'S' && wire[3] == 'H',
        "frame magic is not little-endian FLSH");

    auto* sequence = reinterpret_cast<uint64_t*>(wire.data() + 16);
    *sequence = 2;
    DWORD written = 0;
    Check(WriteFile(client, wire.data(), static_cast<DWORD>(wire.size()), &written, nullptr) &&
        written == wire.size(), "could not write frame bytes back to server");
    Ipc::Frame decoded;
    Check(Ipc::ReadFrame(server, 1, decoded, error, 1000), "native ReadFrame failed");
    Check(decoded.header.sequence == 2 && decoded.header.revision == 7 && decoded.payload == payload,
        "native frame round-trip changed data");
    CloseHandle(client);
    DisconnectNamedPipe(server);
    CloseHandle(server);
}

void TestReadFrameUsesOneDeadline() {
    const std::wstring name = L"\\\\.\\pipe\\FluentShell.NativeDeadlineTests." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE server = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 0, nullptr);
    Check(server != INVALID_HANDLE_VALUE, "deadline test pipe server creation failed");
    if (server == INVALID_HANDLE_VALUE) return;
    HANDLE client = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(client != INVALID_HANDLE_VALUE, "deadline test pipe client creation failed");
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    const BOOL connected = ConnectNamedPipe(server, nullptr);
    Check(connected || GetLastError() == ERROR_PIPE_CONNECTED,
        "deadline test pipe did not connect");

    Ipc::FrameHeader header{};
    header.type = static_cast<uint16_t>(Ipc::MessageType::Heartbeat);
    header.payloadLength = 2;
    header.sequence = 1;
    const std::string payload = "{}";
    std::thread writer([client, header, payload] {
        Sleep(80);
        DWORD written = 0;
        WriteFile(client, &header, sizeof(header), &written, nullptr);
        Sleep(80);
        WriteFile(client, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    });

    const ULONGLONG started = GetTickCount64();
    Ipc::Frame decoded;
    std::wstring error;
    const bool read = Ipc::ReadFrame(server, 0, decoded, error, 120);
    const ULONGLONG elapsed = GetTickCount64() - started;
    Check(!read, "frame header and payload each received a fresh timeout");
    Check(elapsed < 220, "frame read exceeded its total deadline by too much");
    writer.join();
    CloseHandle(client);
    DisconnectNamedPipe(server);
    CloseHandle(server);
}

void TestWriteFrameUsesOneDeadline() {
    const std::wstring name = L"\\\\.\\pipe\\FluentShell.NativeWriteDeadlineTests." +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
    HANDLE server = CreateNamedPipeW(
        name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 1024, 1024, 0, nullptr);
    Check(server != INVALID_HANDLE_VALUE, "write deadline test pipe server creation failed");
    if (server == INVALID_HANDLE_VALUE) return;
    HANDLE client = CreateFileW(
        name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Check(client != INVALID_HANDLE_VALUE, "write deadline test pipe client creation failed");
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return;
    }
    const BOOL connected = ConnectNamedPipe(server, nullptr);
    Check(connected || GetLastError() == ERROR_PIPE_CONNECTED,
        "write deadline test pipe did not connect");

    // Keep the client from draining the server's output so WriteFile enters
    // overlapped pending state. The payload remains within the protocol cap.
    const std::string payload(1024 * 1024, 'x');
    std::wstring error;
    const ULONGLONG started = GetTickCount64();
    const bool written = Ipc::WriteFrame(
        server, Ipc::MessageType::Heartbeat, 1, 0, payload, error, 120);
    const ULONGLONG elapsed = GetTickCount64() - started;
    Check(!written, "backpressured frame write unexpectedly completed");
    Check(elapsed < 500, "frame write exceeded its total deadline by too much");
    CloseHandle(client);
    DisconnectNamedPipe(server);
    CloseHandle(server);
}

void TestScalarContracts() {
    uint64_t value = 0;
    Check(Ipc::TryParseUInt64(L"0", value) && value == 0, "canonical zero u64 rejected");
    Check(Ipc::TryParseUInt64(L"18446744073709551615", value), "maximum u64 rejected");
    Check(!Ipc::TryParseUInt64(L"01", value), "non-canonical leading-zero u64 accepted");
    Check(!Ipc::TryParseUInt64(L"18446744073709551616", value), "overflow u64 accepted");
    const auto nonce = Ipc::NewNonceHex();
    Check(nonce.size() == 32 && nonce.find_first_not_of(L"0123456789abcdef") == std::wstring::npos,
        "nonce is not 128-bit hexadecimal");
    const auto guid = Ipc::NewGuidString();
    GUID parsed{};
    const auto bracedGuid = L"{" + guid + L"}";
    Check(guid.size() == 36 && guid.front() != L'{' && guid.back() != L'}' &&
        guid.find_first_of(L"ABCDEF") == std::wstring::npos &&
        SUCCEEDED(CLSIDFromString(bracedGuid.c_str(), &parsed)),
        "GUID is not canonical lowercase D format");
    Check(Ipc::ProcessCreationTime(GetCurrentProcess()) != 0, "process creation identity is zero");

    const auto gatedCommit = Translation::SerializeSurfaceCommit(
        L"00112233445566778899aabbccddeeff",
        L"11111111-2222-3333-4444-555555555555", 7, true, false);
    Check(gatedCommit.find("\"show\":true") != std::string::npos &&
          gatedCommit.find("\"interactive\":false") != std::string::npos,
        "provisional surface commit did not serialize its interaction gate");
}

void TestSharedFixtures() {
    std::wstring error;
    Translation::HelloMessage hello;
    const auto helloPayload = ReadFixture(L"hello.renderer.json");
    Check(!helloPayload.empty() && Translation::ParseHello(helloPayload, hello, error),
        "renderer hello fixture did not parse");
    Check(hello.role == L"renderer" && hello.processId == 4242 &&
        hello.protocolMajor == Ipc::kProtocolMajor &&
        hello.protocolMinor == Ipc::kProtocolMinor,
        "renderer hello fixture changed meaning");

    Translation::HelloMessage futureHello;
    const auto futureHelloPayload = ReadFixture(L"hello.renderer.future-minor.json");
    Check(!futureHelloPayload.empty() && Translation::ParseHello(
        futureHelloPayload, futureHello, error),
        "future-minor hello fixture did not parse");
    Check(futureHello.protocolMajor == Ipc::kProtocolMajor &&
        futureHello.protocolMinor == Ipc::kProtocolMinor + 1,
        "future-minor hello fixture changed meaning");

    Translation::ActionRequest action;
    const auto actionPayload = ReadFixture(L"action.invoke.json");
    Check(!actionPayload.empty() && Translation::ParseActionInvoke(
        actionPayload, L"00112233445566778899aabbccddeeff", action, error),
        "action.invoke fixture did not parse");
    Check(action.action == L"invoke" && action.nodeId == 1 &&
        action.eventId == 18 && action.expectedRevision == 7,
        "action.invoke fixture changed meaning");

    Translation::ActionRequest unicodeAction;
    const auto unicodePayload = ReadFixture(L"action.invoke.unicode.json");
    Check(!unicodePayload.empty() && Translation::ParseActionInvoke(
        unicodePayload, L"00112233445566778899aabbccddeeff", unicodeAction, error),
        "Unicode action fixture did not parse");
    Check(unicodeAction.action == L"setText" && unicodeAction.nodeId == 9 &&
        unicodeAction.eventId == 19 && unicodeAction.expectedRevision == 8 &&
        unicodeAction.text == L"\u7E41\u9AD4\u4E2D\u6587\u8207 English \u53EF\u4EE5\u5171\u5B58\u3002",
        "Unicode action fixture changed meaning");

    const std::string malformed =
        "{\"messageType\":\"action.invoke\",\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42\",\"eventId\":\"1\","
        "\"expectedRevision\":\"7\",\"action\":\"resize\",\"value\":{\"width\":10}}";
    Translation::ActionRequest rejected;
    Check(!Translation::ParseActionInvoke(
        malformed, L"00112233445566778899aabbccddeeff", rejected, error),
        "partial resize bounds were accepted");

    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42";
    snapshot.revision = 8;
    snapshot.dpi = 96;
    const auto patch = Translation::SerializeWindowPatch(
        L"00112233445566778899AABBCCDDEEFF", 7, snapshot, 18);
    Check(patch.find("\"operations\":[]") != std::string::npos,
        "full snapshot patch did not keep operations empty");
    Check(patch.find("\"property\":\"snapshot\"") == std::string::npos,
        "full snapshot patch still contains the rejected marker operation");
    Check(patch.find("\"eventId\":\"18\"") != std::string::npos,
        "full snapshot patch lost its canonical eventId");
}

void TestStrictMessageValidation() {
    constexpr std::wstring_view nonce = L"00112233445566778899aabbccddeeff";
    std::wstring error;
    Translation::HelloMessage hello;
    const auto validHello = ReadFixture(L"hello.renderer.json");

    Check(!Translation::ParseHello(
        ReplaceOnce(validHello, "\"processId\": 4242", "\"processId\": 1.5"),
        hello, error), "fractional hello processId was accepted");
    Check(!Translation::ParseHello(
        ReplaceOnce(validHello, "\"processId\": 4242", "\"processId\": 4294967296"),
        hello, error), "overflowing hello processId was accepted");
    Check(!Translation::ParseHello(
        ReplaceOnce(validHello, "\"protocolMajor\": 1", "\"protocolMajor\": 1.5"),
        hello, error), "fractional hello major was accepted");
    Check(Translation::ParseHello(
        ReplaceOnce(validHello, "\"protocolMinor\": 1", "\"protocolMinor\": 2"),
        hello, error), "future same-major hello minor was rejected");

    const std::string actionPrefix =
        "{\"messageType\":\"action.invoke\","
        "\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":\"4f17d4bb-b2bf-42b8-a334-2f9ad8d54d42\","
        "\"nodeId\":\"1\",\"eventId\":\"1\",\"expectedRevision\":\"7\",";
    Translation::ActionRequest action;
    Check(!Translation::ParseActionInvoke(
        actionPrefix + "\"action\":\"setCheck\",\"value\":1.5}", nonce, action, error),
        "fractional setCheck value was accepted");
    Check(!Translation::ParseActionInvoke(
        ReplaceOnce(actionPrefix + "\"action\":\"invoke\",\"value\":null}",
            "\"eventId\":\"1\"", "\"eventId\":\"0\""),
        nonce, action, error), "zero action eventId was accepted");

    const std::string maximumText(Ipc::kMaxStringChars, 'a');
    const auto maximumAction = actionPrefix +
        "\"action\":\"setText\",\"value\":\"" + maximumText + "\"}";
    Check(Translation::ParseActionInvoke(maximumAction, nonce, action, error),
        "maximum-size action string was rejected");
    const auto oversizedAction = actionPrefix +
        "\"action\":\"setText\",\"value\":\"" + maximumText + "a\"}";
    Check(!Translation::ParseActionInvoke(oversizedAction, nonce, action, error),
        "oversized action string was accepted");

    const std::string validError =
        "{\"messageType\":\"error\","
        "\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"surfaceId\":null,\"code\":\"protocol_fault\","
        "\"detail\":\"bad frame\",\"fatal\":true}";
    Check(Translation::ParseErrorMessage(validError, nonce, error),
        "valid authenticated error message was rejected");
    Check(!Translation::ParseErrorMessage(
        ReplaceOnce(validError, "00112233445566778899aabbccddeeff",
            "10112233445566778899aabbccddeeff"), nonce, error),
        "error message with wrong nonce was accepted");
    Check(!Translation::ParseErrorMessage(
        ReplaceOnce(validError, ",\"fatal\":true", ""), nonce, error),
        "error message without fatal was accepted");

    const std::string validShutdown =
        "{\"messageType\":\"shutdown\","
        "\"sessionNonce\":\"00112233445566778899aabbccddeeff\","
        "\"reason\":\"renderer shutdown\"}";
    Check(Translation::ParseShutdownMessage(validShutdown, nonce, error),
        "valid authenticated shutdown message was rejected");
    Check(!Translation::ParseShutdownMessage(
        ReplaceOnce(validShutdown, "\"reason\":\"renderer shutdown\"", "\"reason\":\"\""),
        nonce, error), "shutdown message with empty reason was accepted");
}

void TestActionSemanticValidation() {
    Translation::WindowSnapshot snapshot;
    Translation::ControlNode button;
    button.nodeId = 1;
    button.kind = Translation::ControlKind::Button;
    snapshot.nodes.push_back(button);
    Translation::ControlNode check;
    check.nodeId = 2;
    check.kind = Translation::ControlKind::CheckBox;
    snapshot.nodes.push_back(check);
    Translation::ControlNode threeState;
    threeState.nodeId = 3;
    threeState.kind = Translation::ControlKind::ThreeState;
    snapshot.nodes.push_back(threeState);
    Translation::ControlNode radio;
    radio.nodeId = 4;
    radio.kind = Translation::ControlKind::RadioButton;
    snapshot.nodes.push_back(radio);
    Translation::ControlNode edit;
    edit.nodeId = 5;
    edit.kind = Translation::ControlKind::Edit;
    edit.readOnly = true;
    snapshot.nodes.push_back(edit);
    Translation::ControlNode combo;
    combo.nodeId = 6;
    combo.kind = Translation::ControlKind::ComboBox;
    combo.items = { L"one", L"two" };
    snapshot.nodes.push_back(combo);
    Translation::ControlNode editableCombo = combo;
    editableCombo.nodeId = 7;
    editableCombo.editable = true;
    snapshot.nodes.push_back(editableCombo);

    std::wstring error;
    Translation::ActionRequest action;
    action.nodeId = 1;
    action.action = L"invoke";
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "button invoke semantic action was rejected");
    action.nodeId = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "invoke on a checkbox was accepted");
    action.action = L"setCheck";
    action.integerValue = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "two-state checkbox accepted indeterminate value");
    action.nodeId = 3;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "three-state checkbox rejected indeterminate value");
    action.nodeId = 4;
    action.integerValue = 0;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "radio button accepted semantic uncheck");
    action.nodeId = 5;
    action.action = L"setText";
    action.text = L"updated";
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "read-only edit accepted setText");
    action.nodeId = 6;
    action.action = L"select";
    action.integerValue = 2;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "out-of-range selection was accepted");
    action.integerValue = -1;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "selection clear was rejected");
    action.action = L"setText";
    action.text = L"typed";
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "non-editable ComboBox accepted setText");
    action.nodeId = 7;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "editable ComboBox rejected setText");
}

void TestActionRevisionPolicy() {
    Check(Translation::IsRequestSemanticAction(L"invoke"),
        "button invoke must use the latest canonical revision");
    Check(Translation::IsRequestSemanticAction(L"close"),
        "close must remain request semantic");
    Check(Translation::IsRequestSemanticAction(L"move") &&
          Translation::IsRequestSemanticAction(L"resize"),
        "geometry is latest-wins and must not reject a stale revision");
    Check(!Translation::IsRequestSemanticAction(L"activate") &&
          !Translation::IsRequestSemanticAction(L"setText") &&
          !Translation::IsRequestSemanticAction(L"setCheck") &&
          !Translation::IsRequestSemanticAction(L"select") &&
          !Translation::IsRequestSemanticAction(L"menuCommand") &&
          !Translation::IsRequestSemanticAction(L"minimize") &&
          !Translation::IsRequestSemanticAction(L"maximize") &&
          !Translation::IsRequestSemanticAction(L"restore"),
        "property actions must retain stale-revision rejection");
}

void TestErrorScopeParsing() {
    const wchar_t* nonce = L"00112233445566778899aabbccddeeff";
    const std::string prefix =
        R"({"messageType":"error","sessionNonce":"00112233445566778899aabbccddeeff",)";
    std::wstring error;

    bool fatal = true;
    std::wstring surfaceId = L"stale";
    const std::string scoped = prefix +
        R"("surfaceId":"11111111-2222-3333-4444-555555555555",)"
        R"("code":"surface_protocol_fault","detail":"bad patch","fatal":false})";
    Check(Translation::ParseErrorMessage(scoped, nonce, error, &fatal, &surfaceId),
        "surface-scoped error payload did not parse");
    Check(!fatal, "non-fatal error was reported as fatal");
    Check(surfaceId == L"11111111-2222-3333-4444-555555555555",
        "surface-scoped error lost its surfaceId");

    // No surfaceId means the fault cannot be attributed to one window.
    fatal = false;
    surfaceId = L"stale";
    const std::string unscoped = prefix +
        R"("code":"protocol_fault","detail":"bad frame","fatal":true})";
    Check(Translation::ParseErrorMessage(unscoped, nonce, error, &fatal, &surfaceId),
        "session error payload did not parse");
    Check(fatal && surfaceId.empty(),
        "session error must stay fatal and unscoped");

    // An unparseable payload must never be downgraded to a recoverable fault.
    fatal = false;
    surfaceId = L"11111111-2222-3333-4444-555555555555";
    Check(!Translation::ParseErrorMessage(prefix + R"("code":"","detail":"x","fatal":false})",
        nonce, error, &fatal, &surfaceId),
        "empty error code was accepted");
    Check(fatal && surfaceId.empty(),
        "rejected error payload must default to session scope");
}

void TestExpandedControlSerialization() {
    Translation::WindowSnapshot snapshot;
    snapshot.surfaceId = L"11111111-2222-3333-4444-555555555555";
    Translation::ControlNode group;
    group.nodeId = 1;
    group.generation = 1;
    group.kind = Translation::ControlKind::GroupBox;
    group.text = L"Status";
    snapshot.nodes.push_back(group);
    Translation::ControlNode progress;
    progress.nodeId = 2;
    progress.generation = 1;
    progress.kind = Translation::ControlKind::ProgressBar;
    progress.minimum = -10;
    progress.maximum = 30;
    progress.position = 12;
    snapshot.nodes.push_back(progress);
    Translation::ControlNode combo;
    combo.nodeId = 3;
    combo.generation = 1;
    combo.kind = Translation::ControlKind::ComboBox;
    combo.editable = true;
    combo.text = L"custom";
    combo.selectedIndex = 1;
    combo.items = { L"one", L"two" };
    snapshot.nodes.push_back(combo);
    Translation::ControlNode link;
    link.nodeId = 4;
    link.generation = 1;
    link.kind = Translation::ControlKind::SysLink;
    link.text = L"Read the license terms";
    link.automationName = link.text;
    link.items = { L"license terms" };
    snapshot.nodes.push_back(link);
    Translation::ControlNode list;
    list.nodeId = 5;
    list.generation = 1;
    list.kind = Translation::ControlKind::ListView;
    list.columns = { L"Drive", L"Status" };
    list.columnWidths = { 120, 180 };
    list.rows = { { L"C:", L"OK" }, { L"D:", L"Unavailable" } };
    list.items = { L"C:", L"D:" };
    list.selectedIndices = { 0 };
    list.focusedIndex = 0;
    list.multiSelect = true;
    snapshot.nodes.push_back(list);
    Translation::ControlNode status;
    status.nodeId = 6;
    status.generation = 1;
    status.kind = Translation::ControlKind::StatusBar;
    status.items = { L"Ln 1, Col 1", L"100%" };
    status.columnWidths = { 140, 80 };
    snapshot.nodes.push_back(status);

    const auto json = Translation::SerializeWindowOpen(
        L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"kind\":\"groupBox\"") != std::string::npos,
        "GroupBox kind was not serialized");
    Check(json.find("\"kind\":\"progressBar\"") != std::string::npos &&
          json.find("\"minimum\":-10") != std::string::npos &&
          json.find("\"maximum\":30") != std::string::npos &&
          json.find("\"position\":12") != std::string::npos,
        "ProgressBar state was not serialized");
    Check(json.find("\"editable\":true") != std::string::npos &&
          json.find("\"text\":\"custom\"") != std::string::npos &&
          json.find("\"selectedIndex\":1") != std::string::npos,
        "editable ComboBox state was not serialized");
    Check(json.find("\"kind\":\"sysLink\"") != std::string::npos &&
          json.find("\"kind\":\"listView\"") != std::string::npos &&
          json.find("\"columns\":[\"Drive\",\"Status\"]") != std::string::npos &&
          json.find("\"selectedIndices\":[0]") != std::string::npos &&
          json.find("\"kind\":\"statusBar\"") != std::string::npos,
        "expanded common-control state was not serialized");

    const auto editableFingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[2].editable = false;
    Check(editableFingerprint != Translation::SnapshotFingerprint(snapshot),
        "editable state was omitted from the snapshot fingerprint");

    const auto listFingerprint = Translation::SnapshotFingerprint(snapshot);
    snapshot.nodes[4].selectedIndices = { 1 };
    Check(listFingerprint != Translation::SnapshotFingerprint(snapshot),
        "ListView selection was omitted from the snapshot fingerprint");

    Translation::ActionRequest invokeLink;
    invokeLink.action = L"invoke";
    invokeLink.nodeId = 4;
    std::wstring error;
    Check(Translation::ValidateActionForSnapshot(invokeLink, snapshot, error),
        "bounded SysLink invoke was rejected");
    Translation::ActionRequest selectRows;
    selectRows.action = L"setSelection";
    selectRows.nodeId = 5;
    selectRows.integerValues = { 1 };
    Check(Translation::ValidateActionForSnapshot(selectRows, snapshot, error),
        "bounded ListView selection was rejected");
    selectRows.integerValues = { 2 };
    Check(!Translation::ValidateActionForSnapshot(selectRows, snapshot, error),
        "out-of-range ListView selection was accepted");
}

bool CaptureSingleCombo(DWORD comboStyle, Translation::WindowSnapshot& snapshot) {
    HWND window = CreateWindowExW(0, L"Static", L"combo-capture", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return false;
    HWND combo = CreateWindowExW(0, L"ComboBox", L"", WS_CHILD | WS_VISIBLE | comboStyle,
        10, 10, 200, 120, window, reinterpret_cast<HMENU>(100), GetModuleHandleW(nullptr), nullptr);
    if (combo) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"one"));
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"two"));
        SendMessageW(combo, CB_SETCURSEL, 1, 0);
        if ((comboStyle & 0x0003u) == CBS_DROPDOWN) SetWindowTextW(combo, L"custom");
    }
    ShowWindow(window, SW_SHOWNOACTIVATE);
    Translation::CaptureContext context;
    context.surfaceId = L"11111111-2222-3333-4444-555555555555";
    context.generation = 1;
    context.revision = 1;
    std::wstring error;
    const bool captured = combo && Translation::CaptureWindow(window, context, snapshot, error);
    DestroyWindow(window);
    return captured;
}

// The adapter registry is the single place that decides which native class and
// style combinations are inside the bounded translation boundary.  These cases
// pin both directions of that decision so a registry edit cannot silently widen
// or narrow the boundary.
struct AdapterCase final {
    const wchar_t* className;
    DWORD style;
    bool supported;
    Translation::ControlKind kind;
    const char* label;
};

void TestControlAdapterRegistry() {
    HWND root = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"adapter-registry",
        WS_OVERLAPPEDWINDOW, 20, 20, 480, 320, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(root != nullptr, "adapter registry root was not created");
    if (!root) return;

    using Translation::ControlKind;
    const AdapterCase cases[] = {
        { L"Static", SS_LEFT, true, ControlKind::StaticText, "left Static" },
        { L"Static", SS_ETCHEDHORZ, true, ControlKind::Separator, "etched Static" },
        { L"Static", SS_BITMAP, false, ControlKind::StaticText, "bitmap Static" },
        { L"Button", BS_PUSHBUTTON, true, ControlKind::Button, "push Button" },
        { L"Button", BS_AUTOCHECKBOX, true, ControlKind::CheckBox, "check Button" },
        { L"Button", BS_AUTO3STATE, true, ControlKind::ThreeState, "3-state Button" },
        { L"Button", BS_AUTORADIOBUTTON, true, ControlKind::RadioButton, "radio Button" },
        { L"Button", BS_GROUPBOX, true, ControlKind::GroupBox, "GroupBox" },
        { L"Button", BS_GROUPBOX | WS_TABSTOP, false, ControlKind::GroupBox,
          "tab-stop GroupBox" },
        { L"Button", BS_OWNERDRAW, false, ControlKind::Button, "owner-draw Button" },
        { L"Button", BS_PUSHBUTTON | BS_ICON, false, ControlKind::Button, "icon Button" },
        { L"Edit", 0, true, ControlKind::Edit, "Edit" },
        { L"Edit", ES_PASSWORD, true, ControlKind::Password, "password Edit" },
        { L"ComboBox", CBS_DROPDOWNLIST, true, ControlKind::ComboBox, "dropdown-list ComboBox" },
        { L"ComboBox", CBS_SIMPLE | CBS_HASSTRINGS, false, ControlKind::ComboBox,
          "simple ComboBox" },
        { L"ListBox", 0, true, ControlKind::ListBox, "ListBox" },
        { L"ListBox", LBS_EXTENDEDSEL, false, ControlKind::ListBox, "multi-select ListBox" },
        { PROGRESS_CLASSW, 0, true, ControlKind::ProgressBar, "ProgressBar" },
        { PROGRESS_CLASSW, PBS_MARQUEE, false, ControlKind::ProgressBar, "marquee ProgressBar" },
        { STATUSCLASSNAMEW, 0, true, ControlKind::StatusBar, "StatusBar" },
        { STATUSCLASSNAMEW, WS_TABSTOP, false, ControlKind::StatusBar, "tab-stop StatusBar" },
        // SysLink is deliberately absent: its class is only registered by
        // comctl32 v6, which this unmanifested test host does not activate.
        // TestStructuredCommonControlCapture covers the SysLink capture path.
    };

    for (const auto& adapter : cases) {
        HWND child = CreateWindowExW(0, adapter.className, L"probe",
            WS_CHILD | adapter.style, 0, 0, 80, 24, root,
            reinterpret_cast<HMENU>(300), GetModuleHandleW(nullptr), nullptr);
        Check(child != nullptr, adapter.label);
        if (!child) continue;
        ControlKind kind = ControlKind::TreeView;
        std::wstring reason;
        const bool classified = Translation::ClassifyControl(child, kind, reason);
        Check(classified == adapter.supported, adapter.label);
        if (adapter.supported) {
            Check(kind == adapter.kind, adapter.label);
            Check(reason.empty(), adapter.label);
        } else {
            Check(!reason.empty(), adapter.label);
        }
        DestroyWindow(child);
    }

    // An unregistered class is rejected by name so the log identifies it.
    HWND custom = CreateWindowExW(0, L"ScrollBar", L"probe", WS_CHILD,
        0, 0, 40, 40, root, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (custom) {
        ControlKind kind = ControlKind::TreeView;
        std::wstring reason;
        Check(!Translation::ClassifyControl(custom, kind, reason),
            "unregistered class was accepted");
        Check(reason.find(L"unsupported visible control class") != std::wstring::npos,
            "unregistered class rejection does not name the class");
        // Win32 reports the registered casing, so compare the way the registry does.
        wchar_t buffer[Translation::kMaxClassNameChars]{};
        const std::wstring reported(Translation::ClassNameOf(custom, buffer));
        Check(_wcsicmp(reported.c_str(), L"ScrollBar") == 0,
            "ClassNameOf did not report the native class");
        DestroyWindow(custom);
    }

    // A ComboBox owns its edit and dropdown list, so they are part of its adapter
    // contract rather than separate projected nodes.
    HWND combo = CreateWindowExW(0, L"ComboBox", L"",
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | CBS_HASSTRINGS,
        0, 0, 160, 24, root, reinterpret_cast<HMENU>(301),
        GetModuleHandleW(nullptr), nullptr);
    if (combo) {
        COMBOBOXINFO info{ sizeof(info) };
        Check(GetComboBoxInfo(combo, &info) != FALSE, "ComboBox info was unavailable");
        Check(Translation::IsCompositeImplementationChild(info.hwndItem),
            "ComboBox edit child was not treated as an implementation child");
        Check(!Translation::IsCompositeImplementationChild(combo),
            "ComboBox itself was treated as an implementation child");
        DestroyWindow(combo);
    }
    DestroyWindow(root);
}

void TestStructuredCommonControlCapture() {
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, L"Static", L"common-control-capture",
        WS_OVERLAPPEDWINDOW, 20, 20, 640, 420, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "common-control capture root was not created");
    if (!window) return;

    HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
        10, 10, 600, 260, window, reinterpret_cast<HMENU>(200),
        GetModuleHandleW(nullptr), nullptr);
    HWND status = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
        WS_CHILD | WS_VISIBLE, 0, 340, 620, 24, window,
        reinterpret_cast<HMENU>(201), GetModuleHandleW(nullptr), nullptr);
    Check(list != nullptr && status != nullptr,
        "structured common controls were not created");
    if (!list || !status) {
        DestroyWindow(window);
        return;
    }

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.cx = 180;
    column.pszText = const_cast<LPWSTR>(L"Drive");
    SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
    column.cx = 260;
    column.pszText = const_cast<LPWSTR>(L"Status");
    SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = 0;
    item.pszText = const_cast<LPWSTR>(L"C:");
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    item.iItem = 1;
    item.pszText = const_cast<LPWSTR>(L"D:");
    SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
    LVITEMW subItem{};
    subItem.iSubItem = 1;
    subItem.pszText = const_cast<LPWSTR>(L"OK");
    SendMessageW(list, LVM_SETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&subItem));
    subItem.pszText = const_cast<LPWSTR>(L"Unavailable");
    SendMessageW(list, LVM_SETITEMTEXTW, 1, reinterpret_cast<LPARAM>(&subItem));
    LVITEMW selection{};
    selection.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
    selection.state = LVIS_SELECTED | LVIS_FOCUSED;
    SendMessageW(list, LVM_SETITEMSTATE, 1, reinterpret_cast<LPARAM>(&selection));

    int parts[] = { 260, -1 };
    SendMessageW(status, SB_SETPARTS, 2, reinterpret_cast<LPARAM>(parts));
    SendMessageW(status, SB_SETTEXTW, 0, reinterpret_cast<LPARAM>(L"Ln 1, Col 1"));
    SendMessageW(status, SB_SETTEXTW, 1, reinterpret_cast<LPARAM>(L"100%"));
    ShowWindow(window, SW_SHOWNOACTIVATE);

    Translation::CaptureContext context;
    context.surfaceId = L"11111111-2222-3333-4444-555555555555";
    context.generation = 1;
    context.revision = 1;
    Translation::WindowSnapshot snapshot;
    std::wstring error;
    Check(Translation::CaptureWindow(window, context, snapshot, error),
        "bounded ListView/StatusBar window did not capture");
    const auto listNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) {
            return node.kind == Translation::ControlKind::ListView;
        });
    Check(listNode != snapshot.nodes.end() &&
          listNode->columns == std::vector<std::wstring>{ L"Drive", L"Status" } &&
          listNode->rows.size() == 2 && listNode->rows[1][1] == L"Unavailable" &&
          listNode->selectedIndices == std::vector<int>{ 1 } &&
          listNode->focusedIndex == 1 && !listNode->multiSelect,
        "bounded report ListView state was not captured canonically");
    const auto statusNode = std::find_if(snapshot.nodes.begin(), snapshot.nodes.end(),
        [](const Translation::ControlNode& node) {
            return node.kind == Translation::ControlKind::StatusBar;
        });
    Check(statusNode != snapshot.nodes.end() &&
          statusNode->items == std::vector<std::wstring>{ L"Ln 1, Col 1", L"100%" } &&
          statusNode->columnWidths.size() == 2,
        "bounded StatusBar state was not captured canonically");

    SendMessageW(status, SB_SETTEXTW, SBT_OWNERDRAW,
        reinterpret_cast<LPARAM>(reinterpret_cast<void*>(1)));
    Translation::WindowSnapshot rejected;
    context.revision = 2;
    Check(!Translation::CaptureWindow(window, context, rejected, error),
        "owner-draw StatusBar part was accepted");
    DestroyWindow(window);

    HWND virtualWindow = CreateWindowExW(0, L"Static", L"virtual-list",
        WS_OVERLAPPEDWINDOW, 20, 20, 320, 200, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    HWND virtualList = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA,
        10, 10, 280, 120, virtualWindow, reinterpret_cast<HMENU>(202),
        GetModuleHandleW(nullptr), nullptr);
    ShowWindow(virtualWindow, SW_SHOWNOACTIVATE);
    Translation::CaptureContext virtualContext;
    virtualContext.surfaceId = L"22222222-2222-3333-4444-555555555555";
    virtualContext.generation = 1;
    virtualContext.revision = 1;
    Check(virtualList && !Translation::CaptureWindow(
            virtualWindow, virtualContext, rejected, error),
        "virtual ListView was accepted by the bounded adapter");
    DestroyWindow(virtualWindow);
}

void TestEditableComboCaptureBoundary() {
    Translation::WindowSnapshot snapshot;
    Check(CaptureSingleCombo(CBS_DROPDOWNLIST, snapshot) &&
          snapshot.nodes.size() == 1 && !snapshot.nodes[0].editable,
        "standard CBS_DROPDOWNLIST was rejected");
    Check(CaptureSingleCombo(CBS_DROPDOWN | CBS_HASSTRINGS, snapshot) &&
          snapshot.nodes.size() == 1 && snapshot.nodes[0].editable &&
          snapshot.nodes[0].text == L"custom" && snapshot.nodes[0].selectedIndex == 1 &&
          snapshot.nodes[0].items.size() == 2,
        "string-backed editable ComboBox state was not captured canonically");
    Check(!CaptureSingleCombo(CBS_SIMPLE | CBS_HASSTRINGS, snapshot),
        "CBS_SIMPLE was accepted");
    Check(CaptureSingleCombo(CBS_DROPDOWN, snapshot) &&
          snapshot.nodes.size() == 1 && snapshot.nodes[0].editable,
        "Win32-normalized string-backed CBS_DROPDOWN was rejected");
    Check(!CaptureSingleCombo(CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, snapshot),
        "owner-draw ComboBox was accepted");
}

void TestStandardMenuCapture() {
    HWND window = CreateWindowExW(0, L"Static", L"menu-test", WS_OVERLAPPED,
        0, 0, 300, 200, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "menu test window creation failed");
    if (!window) return;
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING | MF_CHECKED, 100, L"&Open\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING | MF_DISABLED, 101, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    SetMenu(window, bar);

    std::vector<Translation::MenuItemSnapshot> menu;
    std::wstring error;
    Check(Translation::CaptureTopLevelMenu(window, menu, error),
        "standard textual menu was rejected");
    Check(menu.size() == 1 && menu[0].itemId == L"0" && menu[0].text == L"&File" &&
        menu[0].items.size() == 3 && menu[0].items[0].commandId == 100 &&
        menu[0].items[0].checked && !menu[0].items[2].enabled,
        "captured menu lost labels, identities, or state");

    AppendMenuW(file, MF_STRING, 100, L"Duplicate");
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "duplicate executable command ID was accepted");
    RemoveMenu(file, 3, MF_BYPOSITION);
    AppendMenuW(file, MF_OWNERDRAW, 102, reinterpret_cast<LPCWSTR>(1));
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "owner-draw menu item was accepted");
    RemoveMenu(file, 3, MF_BYPOSITION);
    MENUINFO callbackInfo{sizeof(callbackInfo)};
    callbackInfo.fMask = MIM_STYLE;
    callbackInfo.dwStyle = MNS_NOTIFYBYPOS;
    SetMenuInfo(file, &callbackInfo);
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "position-callback menu was accepted");
    callbackInfo.dwStyle = 0;
    SetMenuInfo(file, &callbackInfo);
    for (UINT id = 1000; id < 1257; ++id) AppendMenuW(file, MF_STRING, id, L"Item");
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "excessive menu item tree was accepted");

    SetMenu(window, nullptr);
    DestroyMenu(bar);

    HMENU deepBar = CreateMenu();
    HMENU current = CreatePopupMenu();
    AppendMenuW(deepBar, MF_POPUP, reinterpret_cast<UINT_PTR>(current), L"Root");
    for (int depth = 0; depth < 8; ++depth) {
        HMENU next = CreatePopupMenu();
        AppendMenuW(current, MF_POPUP, reinterpret_cast<UINT_PTR>(next), L"Nested");
        current = next;
    }
    AppendMenuW(current, MF_STRING, 200, L"Command");
    SetMenu(window, deepBar);
    Check(!Translation::CaptureTopLevelMenu(window, menu, error),
        "deep menu tree was accepted");
    SetMenu(window, nullptr);
    DestroyMenu(deepBar);
    DestroyWindow(window);
}

void TestMenuActionValidationAndSerialization() {
    Translation::WindowSnapshot snapshot;
    Translation::MenuItemSnapshot root;
    root.itemId = L"0";
    root.kind = Translation::MenuItemKind::Popup;
    root.text = L"&File";
    Translation::MenuItemSnapshot command;
    command.itemId = L"0.0";
    command.kind = Translation::MenuItemKind::Command;
    command.text = L"&Open";
    command.commandId = 77;
    root.items.push_back(command);
    snapshot.menu.push_back(root);
    Translation::ActionRequest action;
    action.action = L"menuCommand";
    action.menuCommandId = 77;
    std::wstring error;
    Check(Translation::ValidateActionForSnapshot(action, snapshot, error),
        "known enabled menu command was rejected");
    action.menuCommandId = 78;
    Check(!Translation::ValidateActionForSnapshot(action, snapshot, error),
        "unknown menu command was accepted");
    snapshot.surfaceId = L"11111111-2222-3333-4444-555555555555";
    const auto json = Translation::SerializeWindowOpen(
        L"00112233445566778899aabbccddeeff", snapshot);
    Check(json.find("\"menu\":[") != std::string::npos &&
        json.find("\"commandId\":77") != std::string::npos &&
        json.find("\"itemId\":\"0.0\"") != std::string::npos,
        "typed menu snapshot was not serialized");
}

} // namespace

int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    INITCOMMONCONTROLSEX controls{ sizeof(controls),
        ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS };
    Check(InitCommonControlsEx(&controls) != FALSE,
        "common-control classes were not initialized");
    TestHeaderValidation();
    TestPipeRoundTrip();
    TestReadFrameUsesOneDeadline();
    TestWriteFrameUsesOneDeadline();
    TestScalarContracts();
    TestSharedFixtures();
    TestStrictMessageValidation();
    TestActionSemanticValidation();
    TestActionRevisionPolicy();
    TestErrorScopeParsing();
    TestExpandedControlSerialization();
    TestControlAdapterRegistry();
    TestEditableComboCaptureBoundary();
    TestStructuredCommonControlCapture();
    TestStandardMenuCapture();
    TestMenuActionValidationAndSerialization();
    if (g_failures != 0) {
        std::cerr << g_failures << " native protocol test(s) failed.\n";
        return 1;
    }
    std::cout << "Native protocol tests passed.\n";
    return 0;
}
