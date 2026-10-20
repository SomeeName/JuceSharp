using Xunit;

namespace JuceSharp.Tests;

public sealed class NativeToneTransactionTests
{
    [Theory]
    [InlineData("dca_plugin_attach_to_input_for_player")]
    [InlineData("dca_plugin_detach_from_input")]
    public void InputPluginStateTransitionIsEntirelyInsideMessageThreadTransaction(string export)
    {
        var source = Read("src/JuceSharp.Native/src/dotcharter_audio.cpp");
        var signature = source.IndexOf("DCA_CALL " + export + "(", StringComparison.Ordinal);
        Assert.True(signature >= 0);
        var methodStart = source.IndexOf('{', signature);
        var methodEnd = ClosingBrace(source, methodStart);
        var method = source[methodStart..(methodEnd + 1)];
        var sync = method.IndexOf("callSync([&]", StringComparison.Ordinal);
        Assert.True(sync >= 0);
        var transactionStart = method.IndexOf('{', sync);
        var transactionEnd = ClosingBrace(method, transactionStart);
        var firstState = method.IndexOf("p->attached", StringComparison.Ordinal);
        var lastState = method.LastIndexOf("p->attached", StringComparison.Ordinal);
        Assert.True(firstState > transactionStart, "idempotence check escaped message-thread serialization");
        Assert.True(lastState < transactionEnd, "attachment publication escaped message-thread serialization");
        Assert.Contains("detachInputPlugin", method[transactionStart..transactionEnd]);
    }

    [Fact]
    public void EngineDestructionRequiresChildrenNativeCompletion()
    {
        var source = Read("src/JuceSharp/JuceAudioEngine.cs");
        Assert.Contains("if (!DisposeChildren(disposing))", source);
        // The WAIT is still there; its deadline now depends on whether anything
        // can still run the destruction (W4-M4). A collected-but-unfinalized
        // child has nobody running its destroy on the disposing thread's behalf,
        // and waiting the 60 s default on each of those froze the host's UI
        // thread for a minute apiece.
        Assert.Contains("entry.Destruction.Wait(ChildDestructionDeadlineMs(reachable))", source);
        Assert.Contains("internal static int ChildDestructionDeadlineMs(bool wrapperReachable)", source);
        Assert.Contains("child.Destruction", source);
        // Completed native teardown, not a disappearing weak wrapper, is the
        // only condition under which a registry entry may be discarded.
        Assert.Contains("if (_children[i].Destruction.IsComplete)", source);
    }

    private static int ClosingBrace(string source, int open)
    {
        var depth = 1;
        for (var i = open + 1; i < source.Length; i++)
        {
            if (source[i] == '{') depth++;
            if (source[i] == '}' && --depth == 0) return i;
        }
        throw new InvalidOperationException();
    }

    private static string Read(string relative)
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var path = Path.Combine(dir.FullName, relative);
            if (File.Exists(path)) return File.ReadAllText(path);
        }
        throw new FileNotFoundException(relative);
    }
}
