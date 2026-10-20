using System;
using System.IO;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// W4-N19: <c>dca_stems_separate_file</c>'s three failure modes are three
/// different problems and must not share one status.
/// <para>
/// "you passed an empty path" is a CALLER BUG. "the weights are not on disk" is
/// a DOWNLOAD the host can go and perform — which is the entire reason
/// <c>DemucsGgmlModelCatalog</c> exists — and it answered
/// <c>DCA_ERR_INVALID_ARG</c> too, so a host could not tell them apart and had
/// no basis for offering the download. It is <c>DCA_ERR_NOT_INITIALIZED</c>
/// now, which is already this export's code for "the backend is absent", i.e.
/// the same actionable meaning: this feature is not ready to run yet.
/// </para>
/// <para>
/// No model file is needed to test any of this, which is the point — every case
/// here fails BEFORE inference starts, so the suite stays fast and needs no
/// multi-hundred-megabyte fixture.
/// </para>
/// </summary>
public sealed class StemsSeparateFileStatusTests
{
    [NativeAvailableFact]
    public void AnEmptyPath_AndAMissingModel_AreDifferentStatuses()
    {
        if (NativeMethods.StemsIsSupported() == 0)
        {
            // The export short-circuits to NOT_INITIALIZED before it ever looks
            // at the arguments, so the distinction below is unobservable.
            Assert.Skip("native stem backend not compiled into this DLL");
            return;
        }

        var dir = Directory.CreateTempSubdirectory("juce-stems-status").FullName;
        try
        {
            var audio = Path.Combine(dir, "in.wav");
            File.WriteAllBytes(audio, Array.Empty<byte>());
            var missingModel = Path.Combine(dir, "definitely-not-here.bin");
            Assert.False(File.Exists(missingModel), "the fixture model must NOT exist");

            // (a) a caller bug: an empty argument.
            var empty = NativeMethods.StemsSeparateFile(audio, dir, "", 1, null, nint.Zero);
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg, empty);

            // (b) every argument well-formed, the weights simply absent.
            var absent = NativeMethods.StemsSeparateFile(audio, dir, missingModel, 1, null, nint.Zero);
            Assert.Equal(NativeMethods.DcaStatus.NotInitialized, absent);

            // THE POINT, stated as its own assertion so a future change that
            // merges them back fails on the reason rather than on a value: a
            // host deciding whether to offer a download must be able to tell
            // these two apart.
            Assert.NotEqual(empty, absent);
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch (IOException) { }
        }
    }

    /// <summary>
    /// And the guard that comes before both: a null path is rejected without
    /// the export reaching any filesystem call. Non-vacuity for the theory
    /// above — it proves the export really is answering about its arguments and
    /// not returning one blanket code for everything.
    /// </summary>
    [NativeAvailableFact]
    public void ANullPath_IsRejectedAsABadArgument()
    {
        var dir = Directory.CreateTempSubdirectory("juce-stems-null").FullName;
        try
        {
            Assert.Equal(NativeMethods.DcaStatus.InvalidArg,
                         NativeMethods.StemsSeparateFile(null!, dir, "m.bin", 1, null, nint.Zero));
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch (IOException) { }
        }
    }
}
