#pragma once

#include <cstdint>

namespace FluentShell::Injector {

// Why a process is, or is not, offered the Bridge.  Decide returns the first refusal in this order.
enum class Eligibility : uint8_t {
    Eligible,
    Unreadable,
    OtherSession,
    Elevated,
    DifferentIntegrity,
    Denied,
    OptedOut,
    AlreadyInjected,
};

// What the watcher knows about one process.  Everything is read through a handle the watcher holds.
struct TargetFacts {
    bool readable = false;          // the handle opened and the image, session and integrity were read
    bool sameSession = false;
    bool higherIntegrity = false;   // above the watcher's own level: an elevated process
    bool sameIntegrity = false;
    bool denied = false;            // shell, security, XAML or FluentShell image name
    bool optedOut = false;          // the user turned this application off
    bool injected = false;          // the watcher already injected this process
};

inline Eligibility Decide(const TargetFacts& facts) noexcept {
    if (!facts.readable) return Eligibility::Unreadable;
    if (!facts.sameSession) return Eligibility::OtherSession;
    if (facts.higherIntegrity) return Eligibility::Elevated;
    if (!facts.sameIntegrity) return Eligibility::DifferentIntegrity;
    if (facts.denied) return Eligibility::Denied;
    if (facts.optedOut) return Eligibility::OptedOut;
    if (facts.injected) return Eligibility::AlreadyInjected;
    return Eligibility::Eligible;
}

// The text the GUI shows for a refusal.  Stable strings: the GUI and the log both read them.
inline const char* ReasonText(Eligibility eligibility) noexcept {
    switch (eligibility) {
    case Eligibility::Eligible: return "eligible";
    case Eligibility::Unreadable: return "process cannot be read (protected, elevated, or exited)";
    case Eligibility::OtherSession: return "another session";
    case Eligibility::Elevated: return "elevated: this user cannot inject into it";
    case Eligibility::DifferentIntegrity: return "different integrity level";
    case Eligibility::Denied: return "shell, security, FluentShell, or the Claude app (denied by name)";
    case Eligibility::OptedOut: return "turned off by the user";
    case Eligibility::AlreadyInjected: return "already projected";
    }
    return "unknown";
}

// A failed injection is retried after 2 s, doubling to a 30 s ceiling, for at most kMaxInjectAttempts
// attempts in all.  Attempts are counted from 1.
inline constexpr unsigned kMaxInjectAttempts = 12;

inline constexpr uint64_t RetryDelayMs(unsigned attempt) noexcept {
    uint64_t delay = 2000;
    for (unsigned step = 1; step < attempt && delay < 30000; ++step) delay *= 2;
    return delay < 30000 ? delay : 30000;
}

inline constexpr bool MayRetry(unsigned attemptsSoFar) noexcept {
    return attemptsSoFar < kMaxInjectAttempts;
}

} // namespace FluentShell::Injector
