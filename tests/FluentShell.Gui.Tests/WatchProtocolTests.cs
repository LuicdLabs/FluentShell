using FluentShell.Gui;
using Xunit;

namespace FluentShell.Gui.Tests;

// The GUI reads the watcher's status lines and drives the Bridges' rollback events.  These tests pin the
// wire format both sides share with src/injector/Watcher.cpp and src/Bridge (RollbackEvent.h).
public class WatchProtocolTests {
    [Fact]
    public void ParsesTargetLineAndUnescapesItsFields() {
        const string line = @"FSW|target|10452|134364574300519149|C:\u005cUsers\u005cUser\u005cApp.exe|pending|dry run: would inject";

        Assert.True(WatchProtocol.TryParse(line, out var watchEvent));
        Assert.NotNull(watchEvent);
        Assert.Equal("target", watchEvent!.Kind);
        Assert.Equal("10452", watchEvent.Fields[0]);
        Assert.Equal(@"C:\Users\User\App.exe", watchEvent.Fields[2]);
        Assert.Equal("pending", watchEvent.Fields[3]);
        Assert.Equal("dry run: would inject", watchEvent.Fields[4]);
    }

    [Fact]
    public void IgnoresLinesThatAreNotStatusLines() {
        Assert.False(WatchProtocol.TryParse("[FluentShell] Projected native window", out _));
        Assert.False(WatchProtocol.TryParse("FSX|target|1", out _));
        Assert.False(WatchProtocol.TryParse(string.Empty, out _));
    }

    [Fact]
    public void UnescapesPipesBackslashesAndNonAscii() {
        Assert.Equal("a|b\\c", WatchProtocol.Unescape(@"a\u007cb\u005cc"));
        Assert.Equal("\u5168", WatchProtocol.Unescape(@"\u5168"));
    }

    [Fact]
    public void DoesNotDecodeAnEscapedBackslashTwice() {
        // The injector escapes a literal backslash-u as \u005cu; that must come back as backslash-u, not a character.
        Assert.Equal(@"\u0041", WatchProtocol.Unescape(@"\u005cu0041"));
    }

    [Theory]
    [InlineData("1a2b")]
    [InlineData("171068e")]
    public void ParsesWindowHandlesAsHex(string text) {
        Assert.True(WatchProtocol.TryParseHandle(text, out var handle));
        Assert.Equal(Convert.ToInt64(text, 16), handle);
    }

    [Fact]
    public void RejectsHandlesThatAreNotHex() {
        Assert.False(WatchProtocol.TryParseHandle("not-a-handle", out _));
    }

    [Fact]
    public void RollbackEventNameMatchesTheBridgeFormat() {
        // Must match FluentShell::Bridge::Translation::RollbackEventName: Local\FluentShell.Rollback.<pid>.<hwnd hex>.
        Assert.Equal(@"Local\FluentShell.Rollback.1234.1a2b", RollbackRequests.NameFor(1234, 0x1a2b));
    }
}
