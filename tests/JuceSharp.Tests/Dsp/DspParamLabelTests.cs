using System.Text;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests.Dsp;

/// <summary>
/// ABI v24 enum value-label catalogue surface: named labels for model-picker
/// params (amp "voicing", cab "model", drive "model") plus the -1 sentinel
/// for continuous params and unknown ids. Pure static catalogue — no engine
/// needed, so these run even on hosts without an audio device.
/// </summary>
public sealed class DspParamLabelTests
{
    private static string Label(string module, string param, int index)
    {
        var len = NativeMethods.DspModuleParamLabel(module, param, index, null, 0);
        Assert.True(len >= 0, $"expected a label for {module}.{param}[{index}]");
        var buf = new byte[len + 1];
        var full = NativeMethods.DspModuleParamLabel(module, param, index, buf, buf.Length);
        Assert.Equal(len, full); // size-then-copy returns the same full length
        Assert.Equal(0, buf[len]); // NUL terminated
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    [Fact]
    public void AmpVoicing_HasNamedLabels_CoveringTheRange()
    {
        var count = NativeMethods.DspModuleParamLabelCount("amp", "voicing");
        Assert.Equal(27, count); // full gear roster: 24 + the three circuit voicings (2026-10)

        // Index 0 keeps its original coefficients (sound); the roster names it.
        Assert.Equal("Blackface Twin 85", Label("amp", "voicing", 0));
        Assert.Equal("Tweed Bass 45", Label("amp", "voicing", 24));
        Assert.Equal("Plexi 100 Circuit", Label("amp", "voicing", 26));

        // The 2026-10 amp switch and the bass head's shift switches are NAMED
        // discretes (a picker), never a plain knob the core rounds.
        Assert.Equal(2, NativeMethods.DspModuleParamLabelCount("amp", "bright"));
        Assert.Equal(2, NativeMethods.DspModuleParamLabelCount("bassamp", "lowShift"));
        Assert.Equal(2, NativeMethods.DspModuleParamLabelCount("bassamp", "highShift"));

        // Every advertised index resolves to a non-empty label.
        for (var i = 0; i < count; i++)
            Assert.False(string.IsNullOrEmpty(Label("amp", "voicing", i)));
    }

    [Fact]
    public void CabAndDrive_ModelParams_AreLabelled()
    {
        Assert.Equal(26, NativeMethods.DspModuleParamLabelCount("cab", "model"));
        Assert.Equal("1x12 Combo", Label("cab", "model", 0));
        Assert.Equal("4x12 V30 Modern", Label("cab", "model", 8));
        // 12-25 (DotCharter #74): appended, so 0-11 keep their index.
        Assert.Equal("4x10 Tweed Open", Label("cab", "model", 12));
        Assert.Equal("1x15 + 1x10 Bass", Label("cab", "model", 25));

        Assert.Equal(4, NativeMethods.DspModuleParamLabelCount("overdrive", "model"));
        Assert.Equal("Generic", Label("overdrive", "model", 0));
        Assert.Equal("Screamer", Label("overdrive", "model", 1));

        Assert.Equal(4, NativeMethods.DspModuleParamLabelCount("distortion", "model"));
        Assert.Equal("Metal", Label("distortion", "model", 2));
    }

    [Fact]
    public void ContinuousParam_ReportsNoLabels()
    {
        // amp "gain" is a plain 0..1 amount — the -1 sentinel means "numeric".
        Assert.Equal(-1, NativeMethods.DspModuleParamLabelCount("amp", "gain"));
        Assert.Equal(-1, NativeMethods.DspModuleParamLabel("amp", "gain", 0, null, 0));

        // gain module's only param is continuous too.
        Assert.Equal(-1, NativeMethods.DspModuleParamLabelCount("gain", "gainDb"));
    }

    [Fact]
    public void UnknownModuleParamOrIndex_ReturnMinusOne()
    {
        Assert.Equal(-1, NativeMethods.DspModuleParamLabelCount("no-such-module", "voicing"));
        Assert.Equal(-1, NativeMethods.DspModuleParamLabelCount("amp", "no-such-param"));

        var count = NativeMethods.DspModuleParamLabelCount("amp", "voicing");
        Assert.Equal(-1, NativeMethods.DspModuleParamLabel("amp", "voicing", count, null, 0)); // one past end
        Assert.Equal(-1, NativeMethods.DspModuleParamLabel("amp", "voicing", -1, null, 0));
    }

    [Fact]
    public void Label_TruncatesButReportsFullLength()
    {
        var full = NativeMethods.DspModuleParamLabel("amp", "voicing", 0, null, 0);
        Assert.True(full > 0);
        var tiny = new byte[2];
        var reported = NativeMethods.DspModuleParamLabel("amp", "voicing", 0, tiny, tiny.Length);
        Assert.Equal(full, reported);       // full length even when truncated
        Assert.Equal(0, tiny[^1]);          // stays NUL terminated
    }
}
