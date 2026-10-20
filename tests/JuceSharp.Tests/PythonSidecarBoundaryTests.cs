using System.Text;
using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The parent/child boundary of the python sidecar: what the child is told
/// (W4-S3, rooted paths) and what comes back (W4-S2, UTF-8; W4-S7, the stderr
/// of a run that timed out).
/// <para>Anything that starts a child touches <c>ChildProcessRegistry</c>'s live
/// set, so this class is in the serialised collection.</para>
/// </summary>
[Collection("StemSeparatorConcurrency")]
public sealed class PythonSidecarBoundaryTests
{
    private static string? SystemPython()
    {
        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrEmpty(path)) return null;
        foreach (var dir in path.Split(Path.PathSeparator))
        {
            foreach (var name in new[] { "python3", "python" })
            {
                var full = Path.Combine(dir, name + (OperatingSystem.IsWindows() ? ".exe" : string.Empty));
                // The Microsoft Store app-execution alias is a 0-byte reparse
                // point that opens the Store instead of running anything.
                if (File.Exists(full) && new FileInfo(full).Length > 0) return full;
            }
        }
        return null;
    }

    // ------------------------------------------------------------ W4-S3

    [Fact]
    public void Every_path_in_the_sidecar_argv_is_rooted()
    {
        var opts = StemSeparationOptions.Default with
        {
            ModelPath = Path.Combine("models", "htdemucs.th"),
            ConfigPath = Path.Combine("models", "htdemucs.yaml"),
        };

        var args = StemSeparator.BuildArgs("script.py", "song.wav", "stems", opts);

        foreach (var flag in new[] { "--input", "--out", "--model-path", "--config" })
        {
            var i = args.IndexOf(flag);
            Assert.True(i >= 0 && i + 1 < args.Count, $"{flag} missing from the argv.");
            Assert.True(Path.IsPathRooted(args[i + 1]),
                $"{flag} was passed '{args[i + 1]}', which the child would resolve against " +
                "the INTERPRETER's directory, not ours.");
        }
    }

    [Fact]
    public async Task A_relative_input_is_rooted_before_it_is_reported_as_missing()
    {
        var separator = new StemSeparator();
        var relative = "definitely-not-here-" + Guid.NewGuid().ToString("N") + ".wav";

        var ex = await Assert.ThrowsAsync<FileNotFoundException>(() =>
            separator.SeparateTrackAsync(relative, Path.GetTempPath(),
                                         ct: TestContext.Current.CancellationToken));

        Assert.NotNull(ex.FileName);
        Assert.True(Path.IsPathRooted(ex.FileName!),
            $"Reported '{ex.FileName}' — the validation and the child argv have to agree on which " +
            "directory a relative path means.");
    }

    // ------------------------------------------------------------ W4-S2

    /// <summary>
    /// The child prints UTF-8 bytes; the parent has to decode them as UTF-8 and
    /// not with whatever the console happens to be set to. The test forces the
    /// console encoding to Latin-1 for the duration, which is what makes the
    /// difference observable on a machine whose ACP is already UTF-8 — on a
    /// cp1252 or cp932 box this is the DEFAULT state.
    /// </summary>
    [Fact]
    public async Task A_non_ascii_stem_line_survives_the_pipe()
    {
        var python = SystemPython();
        Assert.SkipWhen(python is null, "No python3 on PATH; this test drives a real child process.");

        const string wanted = @"STEM vocals C:\Users\Müller\音楽\vocals.wav";
        // Write the bytes explicitly so the child's own default encoding is not
        // part of what is under test — only the parent's decode is.
        var script =
            "import sys; sys.stdout.buffer.write(" +
            "'STEM vocals C:\\\\Users\\\\M\\u00fcller\\\\\\u97f3\\u697d\\\\vocals.wav\\n'.encode('utf-8')); " +
            "sys.stdout.flush()";

        Encoding? restore = null;
        try
        {
            try { restore = Console.OutputEncoding; Console.OutputEncoding = Encoding.Latin1; }
            catch (Exception ex) { Assert.Skip($"Console encoding is not settable here: {ex.Message}"); }

            var (ok, stdout, stderr) = await PythonEnvManager.RunStreaming(
                python!, new[] { "-c", script }, TimeSpan.FromSeconds(30), onLine: null,
                TestContext.Current.CancellationToken);

            Assert.True(ok, $"child failed: {stderr}");
            Assert.Contains(wanted, stdout, StringComparison.Ordinal);
        }
        finally
        {
            if (restore is not null) { try { Console.OutputEncoding = restore; } catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); } }
        }
    }

    // ------------------------------------------------------------ W4-S7

    /// <summary>
    /// A timeout is the case where the child's stderr matters most — a torch
    /// CUDA init that hangs, an OOM kill and a model that never loads all write
    /// there first. The timeout value is an INPUT here; the assertion is on the
    /// content that came back, never on how long anything took.
    /// </summary>
    [Fact]
    public async Task A_timed_out_run_still_reports_what_the_child_said()
    {
        var python = SystemPython();
        Assert.SkipWhen(python is null, "No python3 on PATH; this test drives a real child process.");

        const string marker = "MARKER-cuda-init-stalled";
        var script =
            $"import sys,time; sys.stderr.write('{marker}\\n'); sys.stderr.flush(); time.sleep(600)";

        var (ok, _, stderr) = await PythonEnvManager.RunStreaming(
            python!, new[] { "-c", script }, TimeSpan.FromSeconds(2), onLine: null,
            TestContext.Current.CancellationToken);

        Assert.False(ok);
        Assert.Contains("Timed out", stderr, StringComparison.Ordinal);
        Assert.Contains(marker, stderr, StringComparison.Ordinal);
    }

    // ------------------------------------------------------------ W4-S8

    /// <summary>
    /// <c>Probe()</c> has to answer the question it claims to on POSIX as well —
    /// reading only the field that <c>EnsureInterpreterAsync</c> writes made
    /// <c>IsReady()</c> permanently false there, so a host honouring it showed an
    /// install prompt for a python that was installed.
    /// </summary>
    [Fact]
    public void Probe_finds_a_system_python3_on_posix()
    {
        Assert.SkipWhen(OperatingSystem.IsWindows(),
                        "Windows resolves the embedded interpreter instead; this is the POSIX branch.");
        Assert.SkipWhen(SystemPython() is null, "No python3 on PATH.");

        var mgr = new PythonEnvManager(rootDirectory: Path.Combine(Path.GetTempPath(), "jucesharp-py-" + Guid.NewGuid()));
        var status = mgr.Probe();

        Assert.True(status.InterpreterPresent);
        Assert.NotNull(status.Executable);
    }
}
