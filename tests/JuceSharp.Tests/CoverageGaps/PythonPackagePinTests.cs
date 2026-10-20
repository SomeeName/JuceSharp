using JuceSharp.StemSeparation.Processing;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>
/// <see cref="StemSeparator"/> and <see cref="CkptToOnnxConverter"/> install
/// into the SAME embedded interpreter root. A pin that differs between their
/// two lists is not two opinions, it is one interpreter being re-resolved by
/// whichever ran last — and pip will happily downgrade torch under the other
/// component. The pins were duplicated string literals until
/// <see cref="PythonPackagePins"/> existed.
/// </summary>
public sealed class PythonPackagePinTests
{
    [Fact]
    public void The_two_package_lists_agree_on_every_shared_pin()
    {
        var a = StemSeparator.RequiredPackages;
        var b = CkptToOnnxConverter.RequiredPackages;

        static string Name(string spec)
        {
            var cut = spec.IndexOfAny(['>', '<', '=', '!', '~', ' ']);
            return (cut < 0 ? spec : spec[..cut]).Trim();
        }

        var shared = a.Select(Name).Intersect(b.Select(Name), StringComparer.OrdinalIgnoreCase).ToArray();
        Assert.NotEmpty(shared);
        Assert.Contains("torch", shared, StringComparer.OrdinalIgnoreCase);

        foreach (var name in shared)
        {
            var fromSeparator = a.Single(s => Name(s).Equals(name, StringComparison.OrdinalIgnoreCase));
            var fromConverter = b.Single(s => Name(s).Equals(name, StringComparison.OrdinalIgnoreCase));
            Assert.Equal(fromSeparator, fromConverter);
        }
    }

    [Fact]
    public void Every_pin_comes_from_the_shared_constant_set()
    {
        var known = typeof(PythonPackagePins)
            .GetFields(System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static)
            .Where(f => f.IsLiteral && f.FieldType == typeof(string))
            .Select(f => (string)f.GetRawConstantValue()!)
            .ToHashSet(StringComparer.Ordinal);

        Assert.NotEmpty(known);
        foreach (var spec in StemSeparator.RequiredPackages.Concat(CkptToOnnxConverter.RequiredPackages))
            Assert.Contains(spec, known);
    }

    /// <summary>The co-pin that exists to stop torchaudio's
    /// <c>_torchaudio.pyd</c> binding to a mismatched torch.</summary>
    [Fact]
    public void Torch_and_torchaudio_carry_the_same_bounds()
    {
        Assert.Equal(
            PythonPackagePins.Torch.Replace("torch", string.Empty, StringComparison.Ordinal),
            PythonPackagePins.TorchAudio.Replace("torchaudio", string.Empty, StringComparison.Ordinal));
        Assert.Contains(PythonPackagePins.TorchAudio, StemSeparator.RequiredPackages);
    }
}
