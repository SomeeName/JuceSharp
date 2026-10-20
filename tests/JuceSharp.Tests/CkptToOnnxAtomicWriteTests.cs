using JuceSharp.StemSeparation.Processing;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// W4-S13: a conversion runs for MINUTES inside a child process, so it must not
/// write the <c>.onnx</c> straight to the path every later load reads. A cancel,
/// a crash or a full disk otherwise leaves a truncated model at the canonical
/// location, <c>File.Exists</c> says yes to it, and the user gets an ORT parse
/// error with nothing pointing at the real cause.
/// <para>Drives <c>ConvertCoreAsync</c>'s runner seam, so no interpreter, no
/// 2 GB torch install and no child process are involved.</para>
/// </summary>
public sealed class CkptToOnnxAtomicWriteTests : IDisposable
{
    private readonly string _dir =
        Path.Combine(Path.GetTempPath(), "jucesharp-ckpt-" + Guid.NewGuid().ToString("N"));

    public CkptToOnnxAtomicWriteTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); }
        catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"[test cleanup] {ex.Message}"); }
    }

    private static string OutArg(IReadOnlyList<string> args)
    {
        var i = args.ToList().IndexOf("--out");
        Assert.True(i >= 0 && i + 1 < args.Count, "the argv carries no --out.");
        return args[i + 1];
    }

    /// <summary>Everything in the working directory except the files the test
    /// itself put there — a stray <c>.tmp.*</c> is a failure too.</summary>
    private List<string> Leftovers(params string[] expected)
        => Directory.GetFiles(_dir).Where(f => !expected.Contains(f)).ToList();

    [Fact]
    public async Task A_failed_conversion_leaves_nothing_at_the_canonical_path()
    {
        var ckpt = Path.Combine(_dir, "model.ckpt");
        await File.WriteAllTextAsync(ckpt, "not really a checkpoint", TestContext.Current.CancellationToken);
        var dest = Path.Combine(_dir, "model.onnx");
        var conv = new CkptToOnnxConverter();

        await Assert.ThrowsAsync<InvalidOperationException>(() => conv.ConvertCoreAsync(
            ckpt, null, dest, progress: null,
            runner: async (args, _) =>
            {
                // Half a model, then the exporter dies.
                await File.WriteAllBytesAsync(OutArg(args), new byte[] { 8, 1, 18, 4 }, CancellationToken.None);
                return (false, string.Empty, "RuntimeError: CUDA out of memory");
            },
            ct: TestContext.Current.CancellationToken));

        Assert.False(File.Exists(dest), "a failed conversion published a truncated model.");
        Assert.Empty(Leftovers(ckpt));
    }

    [Fact]
    public async Task A_cancelled_conversion_leaves_nothing_behind()
    {
        var ckpt = Path.Combine(_dir, "model.ckpt");
        await File.WriteAllTextAsync(ckpt, "x", TestContext.Current.CancellationToken);
        var dest = Path.Combine(_dir, "model.onnx");
        var conv = new CkptToOnnxConverter();

        await Assert.ThrowsAsync<OperationCanceledException>(() => conv.ConvertCoreAsync(
            ckpt, null, dest, progress: null,
            runner: async (args, _) =>
            {
                await File.WriteAllBytesAsync(OutArg(args), new byte[] { 8, 1 }, CancellationToken.None);
                throw new OperationCanceledException();
            },
            ct: TestContext.Current.CancellationToken));

        Assert.False(File.Exists(dest));
        Assert.Empty(Leftovers(ckpt));
    }

    [Fact]
    public async Task A_successful_conversion_publishes_the_complete_file()
    {
        var ckpt = Path.Combine(_dir, "model.ckpt");
        await File.WriteAllTextAsync(ckpt, "x", TestContext.Current.CancellationToken);
        var dest = Path.Combine(_dir, "model.onnx");
        var payload = new byte[] { 8, 1, 18, 4, 116, 101, 115, 116 };
        var conv = new CkptToOnnxConverter();

        await conv.ConvertCoreAsync(ckpt, null, dest, progress: null,
            runner: async (args, _) =>
            {
                // The child must NOT be pointed at the canonical path.
                Assert.NotEqual(dest, OutArg(args));
                await File.WriteAllBytesAsync(OutArg(args), payload, CancellationToken.None);
                return (true, string.Empty, string.Empty);
            },
            ct: TestContext.Current.CancellationToken);

        Assert.Equal(payload, await File.ReadAllBytesAsync(dest, TestContext.Current.CancellationToken));
        Assert.Empty(Leftovers(ckpt, dest));
    }
}
