#include "../../src/Bridge/Ipc/FrameCodec.h"
#include "../../src/Bridge/Translation/WindowSnapshot.h"

#include <winrt/base.h>
#include <objbase.h>

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
}

void TestSharedFixtures() {
    std::wstring error;
    Translation::HelloMessage hello;
    const auto helloPayload = ReadFixture(L"hello.renderer.json");
    Check(!helloPayload.empty() && Translation::ParseHello(helloPayload, hello, error),
        "renderer hello fixture did not parse");
    Check(hello.role == L"renderer" && hello.processId == 4242 && hello.protocolMajor == 1,
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
        ReplaceOnce(validHello, "\"protocolMinor\": 0", "\"protocolMinor\": 1"),
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
}

void TestActionRevisionPolicy() {
    Check(Translation::IsRequestSemanticAction(L"invoke"),
        "button invoke must use the latest canonical revision");
    Check(Translation::IsRequestSemanticAction(L"close"),
        "close must remain request semantic");
    Check(!Translation::IsRequestSemanticAction(L"activate") &&
          !Translation::IsRequestSemanticAction(L"setText") &&
          !Translation::IsRequestSemanticAction(L"setCheck") &&
          !Translation::IsRequestSemanticAction(L"select") &&
          !Translation::IsRequestSemanticAction(L"move") &&
          !Translation::IsRequestSemanticAction(L"resize") &&
          !Translation::IsRequestSemanticAction(L"minimize") &&
          !Translation::IsRequestSemanticAction(L"maximize") &&
          !Translation::IsRequestSemanticAction(L"restore"),
        "property actions must retain stale-revision rejection");
}

} // namespace

int wmain() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestHeaderValidation();
    TestPipeRoundTrip();
    TestReadFrameUsesOneDeadline();
    TestWriteFrameUsesOneDeadline();
    TestScalarContracts();
    TestSharedFixtures();
    TestStrictMessageValidation();
    TestActionSemanticValidation();
    TestActionRevisionPolicy();
    if (g_failures != 0) {
        std::cerr << g_failures << " native protocol test(s) failed.\n";
        return 1;
    }
    std::cout << "Native protocol tests passed.\n";
    return 0;
}
