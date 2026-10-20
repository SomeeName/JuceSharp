using System.Diagnostics;
using System.Reflection;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
///     <c>JuceSharp.Dsp</c> is compiled OPTIMIZED in every configuration (its csproj
///     sets <c>Optimize</c> unconditionally). It is live note detection's per-block
///     DSP: unoptimized it ran at ~277 % of real time against ~40 % optimized, so a
///     Debug app's detection fell behind every tick and almost every note read as a
///     miss. This suite itself runs as a Debug build, which is exactly the case.
/// </summary>
public sealed class DspAssemblyOptimizationTests
{
    [Fact]
    public void TheDspAssembly_IsNeverBuiltWithTheJitOptimizerDisabled()
    {
        var assembly = typeof(JuceGuitarAnalyzer).Assembly;
        var debuggable = assembly.GetCustomAttribute<DebuggableAttribute>();

        Assert.False(debuggable?.IsJITOptimizerDisabled ?? false,
            $"{assembly.GetName().Name} was built with JIT optimizations disabled — live note detection "
            + "cannot keep up with real time in that build. Keep <Optimize>true</Optimize> unconditional.");
    }
}
