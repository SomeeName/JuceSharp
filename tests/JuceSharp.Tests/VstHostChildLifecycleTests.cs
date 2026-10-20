using System.Diagnostics;
using System.IO.Pipes;
using System.Runtime.ExceptionServices;
using System.Text;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The sandbox child (<c>juce_vst_host.exe</c>) had no end-to-end test at all:
/// every bridge test drives a managed FAKE child, which has no message thread,
/// no pipe reader and no shutdown sequence. This one starts the real process,
/// speaks its control protocol, and asserts it EXITS.
/// <para>
/// The bug behind it: <c>shutdown()</c> runs on the message thread and joins
/// <c>pipeThread_</c>, while a command inside <c>onMessageThread</c> blocks
/// that pipe thread waiting for the message thread. <c>shuttingDown_</c> only
/// filtered commands PARSED after quit, so a quit arriving during a
/// multi-second <c>load</c> (a cold VST3 scan is 25 s+) wedged the process and
/// the parent had to kill it. <c>requestAsyncQuit</c> now closes the pipe
/// first, and <c>onMessageThread</c> waits in slices and abandons its job when
/// the process is shutting down.
/// </para>
/// <para>
/// TWO TESTS, AND THEY COVER DIFFERENT THINGS. The runtime one below is a
/// lifecycle smoke — connect, load, quit, exit — and it passes against the
/// pre-fix binary too, because the deadlock needs the quit to arrive from a
/// DIFFERENT thread than the one running the command (the parent watchdog, or
/// the OS), which a test cannot arrange against a child it is itself parenting:
/// the control pipe is serial, so a quit sent down it is always processed
/// AFTER the load it was meant to interrupt. It is still worth having — the
/// child process had no end-to-end coverage at all. The DISCRIMINATING half is
/// the source lint, which fails on the unconditional block.
/// </para>
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
[Collection("NativeEngine")]
public sealed class VstHostChildLifecycleTests
{
    /// <summary>
    /// DEADLOCK DETECTOR, NOT A BUDGET. A healthy child connects and exits in
    /// well under a second; the wedge this exists to catch is INFINITE, so the
    /// only job of this number is to fail with a message instead of hanging the
    /// run. It was 20 s and that was too tight: launching a JUCE GUI process
    /// and completing a named-pipe handshake took 23.7 s once on a box that was
    /// simultaneously running a clean C++ rebuild, which failed the whole suite
    /// for a reason that had nothing to do with the child.
    /// </summary>
    private const int ExitTimeoutMs = 90_000;

    [NativeAvailableFact]
    public void AChildAskedToQuitAfterAFailedLoad_ExitsInsteadOfWedging()
    {
        var exe = Path.Combine(AppContext.BaseDirectory, "juce_vst_host.exe");
        if (!File.Exists(exe)) Assert.Skip($"no juce_vst_host.exe next to the test assembly ({exe})");

        var suffix = Guid.NewGuid().ToString("N");
        var pipeName = "dcvb_life_pipe_" + suffix;

        using var server = new NamedPipeServerStream(
            pipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte, PipeOptions.Asynchronous);

        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,   // the child's own log, for a failure message
        };
        foreach (var arg in new[]
                 {
                     "shm=dcvb_life_shm_" + suffix,
                     "req=dcvb_life_req_" + suffix,
                     "resp=dcvb_life_resp_" + suffix,
                     "pipe=" + pipeName,
                     "sr=48000",
                     "block=512",
                     "parent=" + Environment.ProcessId,
                 })
            psi.ArgumentList.Add(arg);

        using var child = Process.Start(psi);
        Assert.NotNull(child);
        var log = new System.Collections.Concurrent.ConcurrentQueue<string>();
        child.ErrorDataReceived += (_, e) => { if (e.Data is not null) log.Enqueue(e.Data); };
        child.BeginErrorReadLine();
        try
        {
            Assert.True(server.WaitForConnectionAsync().Wait(ExitTimeoutMs),
                "the child never connected to the control pipe");

            using var reader = new StreamReader(server, Encoding.UTF8, false, 4096, leaveOpen: true);
            using var writer = new StreamWriter(server, new UTF8Encoding(false), 4096, leaveOpen: true)
            {
                AutoFlush = true,
            };

            var hello = ReadLine(reader);
            Assert.Contains("\"hello\"", hello, StringComparison.Ordinal);
            // Bridge v3 (JuceSharp #28): the child reports the layout it was
            // built against, and DotCharter kills a child whose number differs.
            Assert.Matches("\"bridgeVersion\":\\s*3\\b", hello);

            // A load that FAILS is enough: what is under test is the shutdown
            // that follows a command which went through onMessageThread, not
            // the load itself.
            var missing = Path.Combine(Path.GetTempPath(), "no-such-plugin-" + suffix + ".vst3");
            writer.Write("{\"op\":\"load\",\"id\":1,\"slot\":\"a\",\"path\":\""
                         + missing.Replace("\\", "\\\\") + "\",\"bypass\":false}\n");
            var loadReply = ReadLine(reader);
            Assert.Contains("\"id\": 1", loadReply.Replace("\"id\":1", "\"id\": 1"), StringComparison.Ordinal);

            writer.Write("{\"op\":\"quit\",\"id\":2}\n");
            // Acknowledged BEFORE the shutdown sequence starts, so a failure
            // below is a wedged teardown and not a lost command.
            var quitReply = ReadLine(reader);
            Assert.Contains("\"ok\"", quitReply, StringComparison.Ordinal);

            Assert.True(child.WaitForExit(ExitTimeoutMs),
                $"the child did not exit within {ExitTimeoutMs} ms of being told to quit — "
                + "shutdown() is joining the pipe thread while a command waits on the message thread. "
                + "Child log: " + string.Join(" | ", log));
        }
        finally
        {
            try { if (!child!.HasExited) child.Kill(entireProcessTree: true); } catch { /* best effort */ }
        }
    }

    /// <summary>
    /// Bridge v3 grew the mapping by a MIDI-timing block, and a named mapping
    /// keeps the size it was created with. A v3 child pointed at a mapping a
    /// v2 parent created must refuse it — exit 2 before it ever touches the
    /// control pipe, saying WHY in its log — not view past the end of it or
    /// run a layout the parent does not speak.
    /// </summary>
    [NativeAvailableFact]
    public void AChildGivenABridgeV2Mapping_RefusesItAndExits()
    {
        var exe = Path.Combine(AppContext.BaseDirectory, "juce_vst_host.exe");
        if (!File.Exists(exe)) Assert.Skip($"no juce_vst_host.exe next to the test assembly ({exe})");

        var suffix = Guid.NewGuid().ToString("N");
        var shmName = "dcvb_v2child_shm_" + suffix;
        using var mmf = System.IO.MemoryMappedFiles.MemoryMappedFile.CreateOrOpen(shmName, 197184);
        using (var acc = mmf.CreateViewAccessor(0, 197184))
        {
            acc.Write(0, 0x44435642u);   // 'DCVB'
            acc.Write(4, 2u);            // bridge v2
            acc.Write(8, 8192u);
            acc.Write(12, 2u);
        }

        var psi = new ProcessStartInfo(exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardError = true,
        };
        foreach (var arg in new[]
                 {
                     "shm=" + shmName,
                     "req=dcvb_v2child_req_" + suffix,
                     "resp=dcvb_v2child_resp_" + suffix,
                     "pipe=dcvb_v2child_pipe_" + suffix,   // never created: the child must not get that far
                     "sr=48000",
                     "block=512",
                     "parent=" + Environment.ProcessId,
                 })
            psi.ArgumentList.Add(arg);

        using var child = Process.Start(psi);
        Assert.NotNull(child);
        var log = new System.Collections.Concurrent.ConcurrentQueue<string>();
        child.ErrorDataReceived += (_, e) => { if (e.Data is not null) log.Enqueue(e.Data); };
        child.BeginErrorReadLine();
        try
        {
            Assert.True(child.WaitForExit(ExitTimeoutMs), "the child did not exit after refusing the mapping");
            child.WaitForExit();   // flush the redirected log
            var text = string.Join(" | ", log);
            Assert.Equal(2, child.ExitCode);
            Assert.Contains("bridge layout mismatch: header v2", text, StringComparison.Ordinal);
            Assert.Contains("this build v3", text, StringComparison.Ordinal);
        }
        finally
        {
            try { if (!child!.HasExited) child.Kill(entireProcessTree: true); } catch { /* best effort */ }
        }
    }

    /// <summary>
    /// The startup race (docs/agents/rt-safety-and-tests.md): the control pipe
    /// is a SYNCHRONOUS handle, and Windows serialises synchronous I/O on one
    /// handle, so a <c>hello</c> written AFTER the pipe reader parked in its
    /// blocking <c>ReadFile</c> waited for a read only the parent could
    /// complete — a parent that waits for <c>hello</c> before speaking
    /// deadlocked with it, 4 launches in 70. <c>hello</c> now goes out before
    /// the reader thread exists. A launch loop would be a timing test; the
    /// ORDER is the fix, so the order is what is pinned.
    /// </summary>
    [Fact]
    public void TheChildSaysHelloBeforeItStartsReadingThePipe()
    {
        var src = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/host/VstHostMain.cpp")));
        var init = BodyOf(src, "void initialise");
        var hello = init.IndexOf("writePipeLine(", StringComparison.Ordinal);
        var reader = init.IndexOf("pipeReaderLoop", StringComparison.Ordinal);
        Assert.True(hello >= 0, "initialise() no longer writes hello");
        Assert.True(reader >= 0, "initialise() no longer starts the pipe reader");
        Assert.True(hello < reader,
            "initialise() starts pipeReaderLoop BEFORE writing hello — the synchronous pipe "
            + "handle then serialises the write behind a blocking read only the parent can complete");
    }

    /// <summary>
    /// The half that fails on the old code. Two shapes, both structural:
    /// <c>requestAsyncQuit</c> must close the pipe (so the reader has
    /// something to wake up for), and <c>onMessageThread</c> must not be the
    /// unconditional <c>callFunctionOnMessageThread</c> block — it has to be
    /// able to give up, because <c>shutdown()</c> joins the very thread it is
    /// blocking.
    /// </summary>
    [Fact]
    public void TheChildCannotBlockOnAMessageThreadItsShutdownIsJoiningAgainst()
    {
        var src = StripComments(File.ReadAllText(FindRepoFile(
            "src/JuceSharp.Native/src/host/VstHostMain.cpp")));

        var quit = BodyOf(src, "void requestAsyncQuit");
        Assert.Contains("closePipe()", quit, StringComparison.Ordinal);

        var post = BodyOf(src, "void onMessageThread");
        Assert.DoesNotContain("callFunctionOnMessageThread", post, StringComparison.Ordinal);
        Assert.Contains("shuttingDown_", post, StringComparison.Ordinal);
        Assert.Contains("abandoned", post, StringComparison.Ordinal);

        // Non-vacuity: the extractor really found bodies.
        Assert.Contains("systemRequestedQuit", quit, StringComparison.Ordinal);
        Assert.Contains("callAsync", post, StringComparison.Ordinal);
    }

    private static string BodyOf(string src, string signature)
    {
        var at = src.IndexOf(signature, StringComparison.Ordinal);
        Assert.True(at >= 0, $"{signature} not found in VstHostMain.cpp");
        var open = src.IndexOf('{', at);
        var depth = 0;
        for (var i = open; i < src.Length; i++)
        {
            if (src[i] == '{') depth++;
            else if (src[i] == '}' && --depth == 0) return src.Substring(open, i - open + 1);
        }
        throw new InvalidDataException($"unbalanced braces after {signature}");
    }

    private static string StripComments(string src)
    {
        src = System.Text.RegularExpressions.Regex.Replace(src, @"/\*.*?\*/", " ",
            System.Text.RegularExpressions.RegexOptions.Singleline);
        return System.Text.RegularExpressions.Regex.Replace(src, @"//[^\n]*", " ");
    }

    private static string FindRepoFile(string relative)
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var candidate = Path.Combine(d.FullName, relative.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(candidate)) return candidate;
        }
        throw new InvalidOperationException($"Could not find {relative} above {AppContext.BaseDirectory}.");
    }

    /// <summary>
    /// One line off the control pipe, read on a thread of its own.
    /// <para>
    /// Deliberately NOT <c>Task.Run</c>. The CALLER blocks for the whole read,
    /// so the pooled item and the thread waiting on it were competing for the
    /// same pool: the .NET pool injects roughly one thread per half second above
    /// the core count, and under a loaded gate this bound could expire waiting
    /// for the pool to GROW rather than for the child to answer — which is a
    /// failure message about the child that has nothing to do with the child.
    /// </para>
    /// </summary>
    private static string ReadLine(StreamReader reader)
    {
        string? line = null;
        ExceptionDispatchInfo? failure = null;
        var thread = new Thread(() =>
        {
            try { line = reader.ReadLine(); }
            catch (Exception ex) { failure = ExceptionDispatchInfo.Capture(ex); }
        })
        { IsBackground = true, Name = "vst-host-control-pipe" };
        thread.Start();

        Assert.True(thread.Join(ExitTimeoutMs), "the child never answered on the control pipe");
        failure?.Throw();
        Assert.False(string.IsNullOrEmpty(line), "the child closed the control pipe");
        return line!;
    }
}
