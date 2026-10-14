#include "Watcher.h"

#include "InjectorCore.h"
#include "WatchPolicy.h"

#include "../Common/ProcessPolicy.h"

#include <condition_variable>
#include <cwctype>
#include <deque>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace FluentShell::Injector {

namespace {

constexpr wchar_t kWatchKeyPath[] = L"Software\\FluentShell\\Watch";
constexpr wchar_t kOptOutKeyPath[] = L"Software\\FluentShell\\Watch\\OptOut";
constexpr wchar_t kEnabledValue[] = L"Enabled";
constexpr wchar_t kAcceptedHashValue[] = L"AcceptedRiskSha256";
constexpr wchar_t kWatchMutexName[] = L"Local\\FluentShell.Watch";
constexpr wchar_t kDormantEventName[] = L"Local\\FluentShell.Dormant";
constexpr int kPanicHotkeyId = 1;
constexpr UINT kPanicModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
constexpr UINT kPanicKey = VK_F12;
constexpr UINT kWakeMessage = WM_APP + 1;
constexpr DWORD kTickMs = 1000;

// The text the user accepts.  The GUI shows these exact bytes and records their SHA-256; the watcher
// starts only while the recorded hash equals the hash of this text, so changing the text voids consent.
constexpr char kRiskWarning[] =
    "FluentShell global apply\n"
    "\n"
    "Global apply translates the windows of every eligible application in this Windows session into\n"
    "WinUI windows. It injects FluentShell into each eligible application, and each application gets\n"
    "its own renderer process.\n"
    "\n"
    "Risks:\n"
    "- Applications can behave differently, lose input, or crash. A failure in the translation takes\n"
    "  down the application it is injected into.\n"
    "- Windows of elevated (administrator) applications cannot be reached without elevation. They are\n"
    "  skipped and stay native.\n"
    "- Shell, security, and FluentShell processes, and the Claude desktop app, are excluded by executable\n"
    "  name. A renamed copy of an excluded program is not excluded.\n"
    "- Each injected application gets its own renderer process, so many applications mean many processes.\n"
    "- Only this Windows session is affected, and only while global apply runs. It does not start at\n"
    "  logon.\n"
    "- Global apply stops when you press Ctrl+Alt+Shift+F12, press Restore All, or close FluentShell.\n"
    "- You can turn off any application, or roll back any single window, from the FluentShell GUI.\n"
    "\n"
    "全域套用會把此 Windows 工作階段中每個符合條件的應用程式視窗轉譯為 WinUI 視窗。\n"
    "風險：\n"
    "- 應用程式的行為可能不同，可能遺失輸入或當機。轉譯發生錯誤時，會連同注入的應用程式一起結束。\n"
    "- 以系統管理員權限執行的應用程式視窗無法在不提升權限的情況下存取，因此會略過，維持原生顯示。\n"
    "- 殼層、安全性與 FluentShell 程序，以及 Claude 桌面應用程式，依執行檔名稱排除。更名後的排除程式副本不會被排除。\n"
    "- 每個被注入的應用程式各有一個算繪程序，因此應用程式越多，程序就越多。\n"
    "- 只影響此 Windows 工作階段，且只在全域套用執行期間有效。它不會隨登入自動啟動。\n"
    "- 按下 Ctrl+Alt+Shift+F12、按下「全部還原」或關閉 FluentShell 時，全域套用會停止。\n"
    "- 你可以在 FluentShell GUI 中關閉任一應用程式，或還原任一單一視窗。\n"
    "\n"
    "Continue only if you understand these risks and accept them.\n"
    "只有在你理解並接受上述風險時，才應繼續。\n";

std::string_view RiskWarningText() noexcept {
    return std::string_view(kRiskWarning, sizeof(kRiskWarning) - 1);
}

std::wstring LowerCase(std::wstring text) {
    for (auto& c : text) c = static_cast<wchar_t>(towlower(c));
    return text;
}

// Status fields are escaped to ASCII: \uXXXX for any character outside 0x20-0x7e, and for | and \.
std::wstring Escape(std::wstring_view text) {
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        if (c == L'|' || c == L'\\' || c < 0x20 || c > 0x7e) {
            wchar_t buffer[8]{};
            swprintf_s(buffer, L"\\u%04x", static_cast<unsigned>(c));
            out += buffer;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void Emit(std::initializer_list<std::wstring> fields) {
    std::wstring line = L"FSW";
    for (const auto& field : fields) {
        line.push_back(L'|');
        line += Escape(field);
    }
    std::wcout << line << L'\n' << std::flush;
}

std::wstring Hex(uint64_t value) {
    wchar_t buffer[24]{};
    swprintf_s(buffer, L"%llx", static_cast<unsigned long long>(value));
    return buffer;
}

std::wstring Widen(std::string_view text) {
    std::wstring wide;
    wide.reserve(text.size());
    for (const char c : text) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return wide;
}

std::optional<std::wstring> ReadWatchString(const wchar_t* valueName) {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWatchKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes == 0) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::wstring data(bytes / sizeof(wchar_t), L'\0');
    const LONG status = RegQueryValueExW(key, valueName, nullptr, &type,
        reinterpret_cast<BYTE*>(data.data()), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return std::nullopt;
    while (!data.empty() && data.back() == L'\0') data.pop_back();
    return data;
}

bool ReadWatchEnabled() {
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWatchKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD type = 0;
    DWORD size = sizeof(value);
    const LONG status = RegQueryValueExW(key, kEnabledValue, nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_DWORD && value == 1;
}

// Global apply runs only when the user has enabled it and accepted the current text.
bool IsRiskAccepted() {
    if (!ReadWatchEnabled()) return false;
    const auto accepted = ReadWatchString(kAcceptedHashValue);
    const auto expected = Sha256HexOfBytes(RiskWarningText());
    return accepted && expected && FluentShell::EqualsIgnoreCase(*accepted, *expected);
}

// The applications the user has turned off, by canonical image path.
std::set<std::wstring> ReadOptOutPaths() {
    std::set<std::wstring> paths;
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kOptOutKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return paths;
    }
    for (DWORD index = 0;; ++index) {
        std::wstring name(32767, L'\0');
        DWORD length = static_cast<DWORD>(name.size());
        if (RegEnumValueW(key, index, name.data(), &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        name.resize(length);
        paths.insert(LowerCase(std::move(name)));
    }
    RegCloseKey(key);
    return paths;
}

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
};

// Visible, unowned, titled top-level windows, grouped by owning process.  Cloaked windows are kept: a
// projected window is cloaked by design, and the GUI still needs to find it to roll it back.
std::map<DWORD, std::vector<WindowInfo>> EnumerateWindows() {
    std::map<DWORD, std::vector<WindowInfo>> result;
    EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
        auto& out = *reinterpret_cast<std::map<DWORD, std::vector<WindowInfo>>*>(parameter);
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        wchar_t title[512]{};
        const int length = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
        if (length <= 0) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != 0) out[pid].push_back(WindowInfo{ hwnd, std::wstring(title, length) });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

// What the watcher reads from a process, through a handle it holds while it reads.
struct Probe {
    ULONGLONG created = 0;
    std::wstring image;
    TargetFacts facts;
};

Probe ReadProbe(DWORD pid, std::optional<DWORD> ownSession, std::optional<DWORD> ownIntegrity) {
    Probe probe;
    ProcessHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return probe;
    const auto created = CreationTimeOf(process.get());
    const auto image = CanonicalFilePath(ProcessImagePath(process.get()));
    const auto session = SessionOf(process.get());
    const auto integrity = ProcessIntegrityLevel(process.get());
    if (!created || !image || !session || !integrity || !ownSession || !ownIntegrity) return probe;
    probe.created = *created;
    probe.image = *image;
    probe.facts.readable = true;
    probe.facts.sameSession = *session == *ownSession;
    probe.facts.higherIntegrity = *integrity > *ownIntegrity;
    probe.facts.sameIntegrity = *integrity == *ownIntegrity;
    probe.facts.denied = FluentShell::IsProcessDenied(FluentShell::FileNameOf(*image));
    return probe;
}

struct Job {
    DWORD pid = 0;
    ULONGLONG created = 0;
    std::wstring image;
};

struct Outcome {
    DWORD pid = 0;
    ULONGLONG created = 0;
    InjectResult result;
};

// Runs one injection at a time, off the thread that owns the hotkey and the status lines.  An
// injection can take many seconds, and the panic hotkey must stay responsive while it runs.
class InjectionWorker final {
public:
    explicit InjectionWorker(DWORD wakeThread)
        : wakeThread_(wakeThread), thread_([this] { Run(); }) {}
    ~InjectionWorker() { Stop(); }

    void Submit(Job job) {
        {
            std::scoped_lock lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        ready_.notify_one();
    }

    std::vector<Outcome> Drain() {
        std::scoped_lock lock(mutex_);
        std::vector<Outcome> drained(outcomes_.begin(), outcomes_.end());
        outcomes_.clear();
        return drained;
    }

    void Stop() {
        {
            std::scoped_lock lock(mutex_);
            stop_ = true;
        }
        ready_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    void Run() {
        for (;;) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (stop_) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            Outcome outcome{ job.pid, job.created,
                InjectTarget(job.pid, job.created, job.image, std::nullopt) };
            {
                std::scoped_lock lock(mutex_);
                outcomes_.push_back(std::move(outcome));
            }
            PostThreadMessageW(wakeThread_, kWakeMessage, 0, 0);
        }
    }

    DWORD wakeThread_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    std::deque<Outcome> outcomes_;
    bool stop_ = false;
    std::thread thread_;  // declared last: it starts running in the constructor
};

enum class Phase { Pending, Injected, Skipped, Failed };

const wchar_t* PhaseName(Phase phase) noexcept {
    switch (phase) {
    case Phase::Pending: return L"pending";
    case Phase::Injected: return L"injected";
    case Phase::Skipped: return L"skipped";
    case Phase::Failed: return L"failed";
    }
    return L"unknown";
}

struct Tracked {
    ULONGLONG created = 0;
    std::wstring image;
    std::wstring imageKey;
    Phase phase = Phase::Pending;
    std::wstring reason;
    unsigned attempts = 0;
    ULONGLONG retryAt = 0;
    bool busy = false;
    std::wstring lastTargetLine;
    std::wstring lastWindowLines;
};

class WatchLoop final {
public:
    WatchLoop(DWORD mainThread, bool dryRun)
        : dryRun_(dryRun),
          ownSession_(SessionOf(GetCurrentProcess())),
          ownIntegrity_(ProcessIntegrityLevel(GetCurrentProcess())),
          worker_(std::make_unique<InjectionWorker>(mainThread)) {}

    void Tick() {
        const ULONGLONG now = GetTickCount64();
        const auto optOuts = ReadOptOutPaths();
        const auto windowsByPid = EnumerateWindows();

        for (auto entry = tracked_.begin(); entry != tracked_.end();) {
            if (!entry->second.busy && !windowsByPid.contains(entry->first)) {
                Emit({ L"gone", std::to_wstring(entry->first) });
                entry = tracked_.erase(entry);
            } else {
                ++entry;
            }
        }

        for (const auto& [pid, windows] : windowsByPid) {
            if (pid == GetCurrentProcessId()) continue;
            auto entry = tracked_.find(pid);
            if (entry != tracked_.end() && entry->second.busy) {
                ReportWindows(pid, windows);
                continue;
            }

            Probe probe = ReadProbe(pid, ownSession_, ownIntegrity_);
            if (entry == tracked_.end() || entry->second.created != probe.created) {
                // A new process, or a reused PID: a fresh record.
                Tracked fresh;
                fresh.created = probe.created;
                tracked_[pid] = std::move(fresh);
                entry = tracked_.find(pid);
            }
            Tracked& target = entry->second;
            target.image = probe.image;
            target.imageKey = LowerCase(probe.image);

            probe.facts.optedOut = optOuts.contains(target.imageKey);
            probe.facts.injected = target.phase == Phase::Injected;
            const Eligibility decision = Decide(probe.facts);

            if (decision == Eligibility::Eligible && dryRun_) {
                if (target.phase != Phase::Injected) {
                    target.phase = Phase::Pending;
                    target.reason = L"dry run: would inject";
                }
            } else if (decision == Eligibility::Eligible) {
                const bool retryDue = target.phase == Phase::Failed &&
                                      MayRetry(target.attempts) && now >= target.retryAt;
                if (target.phase == Phase::Pending || target.phase == Phase::Skipped || retryDue) {
                    target.phase = Phase::Pending;
                    target.busy = true;
                    worker_->Submit(Job{ pid, target.created, target.image });
                }
            } else if (decision == Eligibility::AlreadyInjected) {
                // Nothing to do: the Bridge is still in the process.
            } else if (target.phase == Phase::Injected) {
                // Keep the projection's record; the user decides what to roll back.
                if (decision == Eligibility::OptedOut) {
                    target.reason = L"turned off by the user; roll back its windows in the GUI";
                }
            } else {
                target.phase = Phase::Skipped;
                target.reason = Widen(ReasonText(decision));
            }

            ReportTarget(pid, target);
            ReportWindows(pid, windows);
        }
    }

    void Drain() {
        const ULONGLONG now = GetTickCount64();
        for (auto& outcome : worker_->Drain()) {
            auto entry = tracked_.find(outcome.pid);
            if (entry == tracked_.end() || entry->second.created != outcome.created) continue;
            Tracked& target = entry->second;
            target.busy = false;
            switch (outcome.result.status) {
            case InjectStatus::Injected:
                target.phase = Phase::Injected;
                target.reason.clear();
                target.attempts = 0;
                break;
            case InjectStatus::Refused:
                target.phase = Phase::Skipped;
                target.reason = outcome.result.reason;
                break;
            case InjectStatus::Failed:
                target.attempts += 1;
                target.phase = Phase::Failed;
                target.reason = outcome.result.reason;
                target.retryAt = now + RetryDelayMs(target.attempts);
                break;
            }
            ReportTarget(outcome.pid, target);
        }
    }

    void Shutdown() { worker_->Stop(); }

private:
    void ReportTarget(DWORD pid, Tracked& target) {
        const std::wstring line = std::to_wstring(pid) + L"|" + std::to_wstring(target.created) + L"|" +
                                  target.image + L"|" + PhaseName(target.phase) + L"|" + target.reason;
        if (line == target.lastTargetLine) return;
        target.lastTargetLine = line;
        Emit({ L"target", std::to_wstring(pid), std::to_wstring(target.created), target.image,
            PhaseName(target.phase), target.reason });
    }

    void ReportWindows(DWORD pid, const std::vector<WindowInfo>& allWindows) {
        auto entry = tracked_.find(pid);
        if (entry == tracked_.end()) return;
        // Only a target the watcher manages has windows in the GUI.  A denied process (the renderer's proxy
        // carries its application's title), an elevated one, or any other skipped process would otherwise list
        // windows that global apply does not own.  An empty list is still reported, so rows drop when a target
        // stops being managed.
        static const std::vector<WindowInfo> kNone;
        const bool managed = entry->second.phase == Phase::Injected || entry->second.phase == Phase::Pending;
        const auto& windows = managed ? allWindows : kNone;
        std::wstring signature;
        for (const auto& window : windows) {
            signature += Hex(reinterpret_cast<uintptr_t>(window.hwnd)) + L"|" + window.title + L"\n";
        }
        if (signature == entry->second.lastWindowLines) return;
        entry->second.lastWindowLines = signature;
        // The header replaces every window the GUI holds for this process, including when none are left.
        Emit({ L"windows", std::to_wstring(pid), std::to_wstring(windows.size()) });
        for (const auto& window : windows) {
            Emit({ L"window", std::to_wstring(pid), Hex(reinterpret_cast<uintptr_t>(window.hwnd)), window.title });
        }
    }

    bool dryRun_ = false;
    std::optional<DWORD> ownSession_;
    std::optional<DWORD> ownIntegrity_;
    std::map<DWORD, Tracked> tracked_;
    std::unique_ptr<InjectionWorker> worker_;
};

// The panic path: every Bridge that watches the shared event restores its windows and offers none
// until the event is reset (the GUI's Resume).  The event is created here only if no Bridge holds it.
void SignalDormant() {
    HANDLE dormant = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDormantEventName);
    if (!dormant) dormant = CreateEventW(nullptr, TRUE, FALSE, kDormantEventName);
    if (dormant) {
        SetEvent(dormant);
        CloseHandle(dormant);
    }
}

} // namespace

int RunWatch(bool dryRun) {
    if (!IsRiskAccepted()) {
        std::wcerr << L"Global apply is off. Accept the risk warning in FluentShell.Gui.exe first.\n";
        return 6;
    }
    HANDLE mutex = CreateMutexW(nullptr, FALSE, kWatchMutexName);
    if (!mutex) {
        std::wcerr << L"Cannot create the watcher mutex: " << GetLastError() << L"\n";
        return 7;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        std::wcerr << L"Global apply is already running.\n";
        return 7;
    }
    if (!RegisterHotKey(nullptr, kPanicHotkeyId, kPanicModifiers, kPanicKey)) {
        std::wcerr << L"The panic hotkey (Ctrl+Alt+Shift+F12) is unavailable; global apply will not start.\n";
        CloseHandle(mutex);
        return 8;
    }

    std::wstring stopReason = L"stopped";
    {
        WatchLoop loop(GetCurrentThreadId(), dryRun);
        Emit({ L"started", std::to_wstring(GetCurrentProcessId()), dryRun ? L"dry-run" : L"apply" });
        for (;;) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, kTickMs, QS_ALLINPUT);
            bool panic = false;
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_HOTKEY && message.wParam == kPanicHotkeyId) panic = true;
            }
            if (panic) {
                stopReason = L"panic";
                break;
            }
            if (!IsRiskAccepted()) {
                stopReason = L"disabled";
                break;
            }
            loop.Drain();
            loop.Tick();
        }
        if (stopReason == L"panic") SignalDormant();
        loop.Shutdown();
    }
    UnregisterHotKey(nullptr, kPanicHotkeyId);
    CloseHandle(mutex);
    Emit({ L"stopped", stopReason });
    return 0;
}

int PrintRiskWarning() {
    const std::string_view text = RiskWarningText();
    DWORD written = 0;
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE ||
        !WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)) {
        return 5;
    }
    return written == text.size() ? 0 : 5;
}

} // namespace FluentShell::Injector
