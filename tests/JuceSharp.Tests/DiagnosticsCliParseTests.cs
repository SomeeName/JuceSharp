using JuceSharp.Diagnostics;
using Xunit;

namespace JuceSharp.Tests;

public sealed class DiagnosticsCliParseTests
{
    [Fact]
    public void Parse_requires_input()
    {
        Assert.Throws<ArgumentException>(() => DiagnosticsOptions.Parse(["--out", "x"]));
    }

    [Fact]
    public void Parse_rejects_unknown_flag()
    {
        Assert.Throws<ArgumentException>(() =>
            DiagnosticsOptions.Parse(["--input", "a.wav", "--bogus"]));
    }

    [Fact]
    public void Parse_rejects_value_less_flag_at_end()
    {
        Assert.Throws<ArgumentException>(() =>
            DiagnosticsOptions.Parse(["--input", "a.wav", "--model"]));
    }

    [Fact]
    public void Parse_populates_defaults_and_overrides()
    {
        var o = DiagnosticsOptions.Parse(
        [
            "--input", "song.wav",
            "--out", "outdir",
            "--model", "htdemucs_6s",
            "--device", "cpu",
            "--shifts", "2",
            "--overlap", "0.4",
            "--max-concurrent", "3",
            "--skip-bootstrap",
            "--quiet",
        ]);
        Assert.Equal("song.wav", o.InputPath);
        Assert.Equal("outdir", o.OutputDirectory);
        Assert.Equal("htdemucs_6s", o.Model);
        Assert.Equal("cpu", o.Device);
        Assert.Equal(2, o.Shifts);
        Assert.Equal(0.4, o.Overlap, 3);
        Assert.Equal(3, o.MaxConcurrent);
        Assert.True(o.SkipBootstrap);
        Assert.True(o.Quiet);

        var backend = o.ToBackendOptions();
        Assert.Equal("htdemucs_6s", backend.Model);
        Assert.Equal(3, backend.MaxConcurrentSeparations);
        Assert.True(backend.SkipPackageBootstrap);
    }

    [Fact]
    public void Parse_defaults_overlap_and_shifts()
    {
        var o = DiagnosticsOptions.Parse(["--input", "a.wav"]);
        Assert.Equal(1, o.Shifts);
        Assert.Equal(0.25, o.Overlap, 3);
        Assert.Equal(1, o.MaxConcurrent);
        Assert.False(o.SkipBootstrap);
        Assert.False(o.Quiet);
        Assert.Null(o.Timeout);
        Assert.Null(o.Stems);
        Assert.False(o.HelpRequested);
        Assert.False(o.VersionRequested);
        Assert.False(o.ProbeOnly);
        // An unspecified timeout must still reach the backend as the
        // documented 4-hour cap, not as TimeSpan.Zero.
        Assert.Equal(TimeSpan.FromHours(4), o.ToBackendOptions().Timeout);
    }

    // ------------------------------------------------------------------
    // A malformed NUMBER used to escape as FormatException — which is not an
    // ArgumentException, so Main's catch missed it and the process died with
    // 0xE0434352 instead of exiting 2.
    // ------------------------------------------------------------------

    [Theory]
    [InlineData("--shifts", "two")]
    [InlineData("--shifts", "1.5")]
    [InlineData("--overlap", "half")]
    [InlineData("--overlap", "")]
    [InlineData("--max-concurrent", "lots")]
    [InlineData("--timeout", "soon")]
    public void Parse_reports_a_malformed_number_as_an_argument_error(string flag, string value)
    {
        var ex = Assert.Throws<ArgumentException>(() =>
            DiagnosticsOptions.Parse(["--input", "a.wav", flag, value]));
        Assert.Contains(flag, ex.Message, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("--shifts", "-1")]
    [InlineData("--overlap", "1")]
    [InlineData("--overlap", "-0.1")]
    [InlineData("--max-concurrent", "0")]
    [InlineData("--timeout", "0")]
    [InlineData("--timeout", "-5")]
    public void Parse_rejects_an_out_of_range_value(string flag, string value)
    {
        Assert.Throws<ArgumentException>(() =>
            DiagnosticsOptions.Parse(["--input", "a.wav", flag, value]));
    }

    [Fact]
    public void Parse_reads_timeout_in_minutes()
    {
        var o = DiagnosticsOptions.Parse(["--input", "a.wav", "--timeout", "90"]);
        Assert.Equal(TimeSpan.FromMinutes(90), o.Timeout);
        Assert.Equal(TimeSpan.FromMinutes(90), o.ToBackendOptions().Timeout);
    }

    [Fact]
    public void Parse_reads_a_stem_subset()
    {
        var o = DiagnosticsOptions.Parse(["--input", "a.wav", "--stems", "vocals, drums ,other"]);
        Assert.Equal(new[] { "vocals", "drums", "other" }, o.Stems);
        Assert.Equal(new[] { "vocals", "drums", "other" }, o.ToBackendOptions().Stems);
    }

    [Fact]
    public void Parse_rejects_an_empty_stem_list()
    {
        Assert.Throws<ArgumentException>(() =>
            DiagnosticsOptions.Parse(["--input", "a.wav", "--stems", " , ,"]));
    }

    // ------------------------------------------------------------------
    // --help / --version / --probe are complete requests on their own. The
    // parser's `--help` arm used to be unreachable, because Main short-
    // circuited on the flag and Parse still demanded --input.
    // ------------------------------------------------------------------

    [Theory]
    [InlineData("--help")]
    [InlineData("-h")]
    public void Help_parses_without_an_input(string flag)
    {
        var o = DiagnosticsOptions.Parse([flag]);
        Assert.True(o.HelpRequested);
        Assert.False(o.VersionRequested);
        Assert.False(o.ProbeOnly);
    }

    [Fact]
    public void Version_parses_without_an_input()
    {
        var o = DiagnosticsOptions.Parse(["--version"]);
        Assert.True(o.VersionRequested);
        Assert.False(o.HelpRequested);
    }

    [Fact]
    public void Probe_parses_without_an_input()
    {
        var o = DiagnosticsOptions.Parse(["--probe"]);
        Assert.True(o.ProbeOnly);
        Assert.False(o.VersionRequested);
    }

    /// <summary>Help wins over the other two so a confused invocation is
    /// answered with the documentation rather than a version banner.</summary>
    [Fact]
    public void Help_takes_precedence_over_version_and_probe()
    {
        var o = DiagnosticsOptions.Parse(["--help", "--version", "--probe"]);
        Assert.True(o.HelpRequested);
        Assert.False(o.VersionRequested);
        Assert.False(o.ProbeOnly);
    }

    [Fact]
    public void Usage_text_documents_every_flag_the_parser_accepts()
    {
        var w = new StringWriter();
        Program.PrintUsage(w);
        var usage = w.ToString();

        foreach (var flag in new[]
                 {
                     "--input", "--out", "--model", "--device", "--shifts", "--overlap",
                     "--stems", "--timeout", "--max-concurrent", "--model-path", "--config",
                     "--skip-bootstrap", "--probe", "--quiet", "--version", "--help",
                 })
        {
            Assert.Contains(flag, usage, StringComparison.Ordinal);
        }
    }
}
