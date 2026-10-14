using System.Threading;

namespace FluentShell.Gui;

// The session-wide dormancy event the Bridges read.  Set: every window goes native and stays native.
// Reset: the Bridges offer the windows again (except windows a user rolled back, which stay native until
// the application restarts).
internal static class Dormancy {
    private const string EventName = @"Local\FluentShell.Dormant";

    public static void SetDormant() {
        using var dormant = new EventWaitHandle(initialState: false, EventResetMode.ManualReset, EventName);
        dormant.Set();
    }

    public static void Resume() {
        using var dormant = new EventWaitHandle(initialState: false, EventResetMode.ManualReset, EventName);
        dormant.Reset();
    }
}

// A projected window's rollback request.  A Bridge creates Local\FluentShell.Rollback.<pid>.<hwnd> while the
// root window is projected and closes it when the window stops projecting, so the event's presence is the
// projection state, and signaling it rolls the window back.
internal static class RollbackRequests {
    public static string NameFor(uint processId, long handle) => $@"Local\FluentShell.Rollback.{processId}.{handle:x}";

    public static bool IsProjected(uint processId, long handle) {
        try {
            using var request = EventWaitHandle.OpenExisting(NameFor(processId, handle));
            return true;
        } catch (WaitHandleCannotBeOpenedException) {
            return false;
        }
    }

    public static bool Request(uint processId, long handle) {
        try {
            using var request = EventWaitHandle.OpenExisting(NameFor(processId, handle));
            request.Set();
            return true;
        } catch (WaitHandleCannotBeOpenedException) {
            return false;
        }
    }
}
