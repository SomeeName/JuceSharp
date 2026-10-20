using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The gate-hygiene pin for the two IDMT evaluation harnesses
/// (<see cref="CqtDatasetEvalTests"/>, <see cref="CqtStreamingEvalTests"/>).
/// They are documented — in three places — as opt-in, and a ~10-minute dataset
/// sweep that ends in an assertion failure or a dead test host has no business
/// starting because of who is logged in.
/// </summary>
public sealed class IdmtDatasetTests
{
    [Fact]
    public void ResolveRoot_WithoutTheEnvVar_IsNull_SoTheEvalsSkip()
    {
        // No fallback path of any kind — not a developer's checkout, not a
        // sibling directory, not the working directory.
        Assert.Null(IdmtDataset.ResolveRoot(_ => null));
        Assert.Null(IdmtDataset.ResolveRoot(_ => ""));
        Assert.Null(IdmtDataset.ResolveRoot(_ => "   "));
    }

    [Fact]
    public void ResolveRoot_IgnoresAnEnvVarPointingAtNothing()
        => Assert.Null(IdmtDataset.ResolveRoot(
            _ => Path.Combine(Path.GetTempPath(), "no-such-idmt-" + Guid.NewGuid().ToString("N"))));

    [Fact]
    public void ResolveRoot_HonoursGndIdmtRoot_AndReadsNoOtherVariable()
    {
        var dir = Path.Combine(Path.GetTempPath(), "idmt-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            var asked = new List<string>();
            var resolved = IdmtDataset.ResolveRoot(name =>
            {
                asked.Add(name);
                return name == "GND_IDMT_ROOT" ? dir : null;
            });
            Assert.Equal(dir, resolved);
            Assert.Equal(new[] { "GND_IDMT_ROOT" }, asked);
        }
        finally { try { Directory.Delete(dir); } catch (IOException) { /* best effort */ } }
    }

    [Fact]
    public void TheEvalsThemselvesGoThroughResolveRoot_SoTheGateIsQuietByDefault()
    {
        // Belt and braces: the parameterless overload is what the two [Fact]s
        // call, and it must agree with the injected one for the real environment.
        var expected = IdmtDataset.ResolveRoot(Environment.GetEnvironmentVariable);
        Assert.Equal(expected, IdmtDataset.ResolveRoot());
    }
}
