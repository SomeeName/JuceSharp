using Xunit;

namespace JuceSharp.Tests;

public class JuceMixerTests
{
    [Fact]
    public void ComputeMuteStates_NoSolo_RespectsUserMute()
    {
        var muted = new[] { false, true,  false };
        var solo  = new[] { false, false, false };

        var result = JuceMixer.ComputeMuteStates(muted, solo);

        Assert.Equal(new[] { false, true, false }, result);
    }

    [Fact]
    public void ComputeMuteStates_OneSolo_MutesEveryoneElse()
    {
        var muted = new[] { false, false, false };
        var solo  = new[] { false, true,  false };

        var result = JuceMixer.ComputeMuteStates(muted, solo);

        Assert.Equal(new[] { true, false, true }, result);
    }

    [Fact]
    public void ComputeMuteStates_SoloDoesNotOverrideUserMuteOnSoloedChannel()
    {
        // A channel can be both soloed and user-muted -> still muted.
        var muted = new[] { true,  false };
        var solo  = new[] { true,  false };

        var result = JuceMixer.ComputeMuteStates(muted, solo);

        Assert.Equal(new[] { true, true }, result);
    }

    [Fact]
    public void ComputeMuteStates_MultipleSolos_OnlySoloedChannelsAudible()
    {
        var muted = new[] { false, false, false, false };
        var solo  = new[] { true,  false, true,  false };

        var result = JuceMixer.ComputeMuteStates(muted, solo);

        Assert.Equal(new[] { false, true, false, true }, result);
    }

    [Fact]
    public void ComputeMuteStates_EmptyInput_ReturnsEmpty()
    {
        var result = JuceMixer.ComputeMuteStates(Array.Empty<bool>(), Array.Empty<bool>());
        Assert.Empty(result);
    }

    [Fact]
    public void ComputeMuteStates_LengthMismatch_Throws()
    {
        Assert.Throws<ArgumentException>(() =>
            JuceMixer.ComputeMuteStates(new[] { true }, new[] { false, false }));
    }

    [Fact]
    public void ComputeMuteStates_NullArgs_Throws()
    {
        Assert.Throws<ArgumentNullException>(() =>
            JuceMixer.ComputeMuteStates(null!, Array.Empty<bool>()));
        Assert.Throws<ArgumentNullException>(() =>
            JuceMixer.ComputeMuteStates(Array.Empty<bool>(), null!));
    }

    [Fact]
    public void Dispose_OnEmptyMixer_DoesNotThrow_AndPreventsFurtherUse()
    {
        var mixer = new JuceMixer();
        mixer.Dispose();
        mixer.Dispose(); // idempotent
        Assert.Throws<ObjectDisposedException>(() => mixer.Play());
    }
}

/// <summary>
/// W4-M3 and W4-C3: the two things <see cref="JuceMixer" /> said about itself
/// that were not true. Real sources rather than the fake, because
/// <see cref="JuceMixerChannel.Source" /> is the concrete
/// <see cref="JuceAudioSource" /> — the mixer is a facade over native handles.
/// </summary>
[Collection("NullDevice")]
public sealed class JuceMixerSoloAndTransportTests : IDisposable
{
    private readonly NullDeviceScope _scope = new();
    public void Dispose() => _scope.Dispose();

    /// <summary>
    ///     W4-M3. <c>Remove</c> refreshed the solo muting and every flag write
    ///     went through <c>SetChannelFlag</c> → <c>RefreshSoloMuting</c>;
    ///     <c>Add</c> did neither. So <see cref="JuceMixerChannel.Solo" />'s
    ///     documented invariant — "while any channel in the mixer is soloed,
    ///     every channel that is not is muted" — was false the moment a channel
    ///     arrived during a solo. That is the ORDINARY case for a
    ///     stem-separation UI that adds stems as they finish decoding: the vocal
    ///     solo is up, drums land, and drums play.
    /// </summary>
    [NativeAvailableFact]
    public void AChannelAddedDuringASolo_IsMutedByIt()
    {
        using var engine = NullDevice.CreateEngine();
        var wav = TinyWav.Create();
        using var mixer = new JuceMixer();
        using var vocals = engine.OpenFile(wav);
        using var drums = engine.OpenFile(wav);

        var v = mixer.Add("vocals", vocals);
        v.Solo = true;
        Assert.False(v.Source.Mute);

        var d = mixer.Add("drums", drums);

        Assert.True(d.Source.Mute,
            "a stem added while another is soloed plays over it — JuceMixer.Add never re-applied the solo");
        Assert.False(v.Source.Mute);
    }

    /// <summary>…and clearing the solo restores the late arrival, so the refresh
    /// on Add is a real fold into the mixer's state rather than a one-off
    /// mute.</summary>
    [NativeAvailableFact]
    public void ClearingTheSolo_UnmutesAChannelThatArrivedDuringIt()
    {
        using var engine = NullDevice.CreateEngine();
        var wav = TinyWav.Create();
        using var mixer = new JuceMixer();
        using var a = engine.OpenFile(wav);
        using var b = engine.OpenFile(wav);

        var first = mixer.Add("a", a);
        first.Solo = true;
        var late = mixer.Add("b", b);
        Assert.True(late.Source.Mute);

        first.Solo = false;

        Assert.False(late.Source.Mute);
        Assert.False(first.Source.Mute);
    }

    /// <summary>
    ///     W4-C3. <c>Play()</c>'s summary said "every non-muted source"; it plays
    ///     every source, and that is CORRECT — mute is a gain here
    ///     (<see cref="JuceAudioSource.Mute" /> writes <c>source_set_volume(0)</c>),
    ///     not a transport state, so a muted stem has to keep running or it
    ///     drifts. The DOC was the wrong half, and a reader chasing "why is a
    ///     muted stem out of sync" would have trusted it and looked elsewhere.
    ///     <para>
    ///         Read off the method's own brace-matched body and its doc comment,
    ///         because there is nothing to observe: the ABI exports no
    ///         <c>is_playing</c>, and <see cref="JuceMixerChannel.Source" /> is the
    ///         concrete native-backed type, so no fake can record the call either.
    ///     </para>
    /// </summary>
    [Fact]
    public void Play_StartsEverySource_AndItsDocNoLongerSaysOtherwise()
    {
        var text = WrapperLifetimeAuditTests.ReadRepoFile("src/JuceSharp/JuceMixer.cs");

        var at = text.IndexOf("public void Play()", StringComparison.Ordinal);
        Assert.True(at >= 0, "JuceMixer.Play() is gone — repoint this lint.");

        var open = text.IndexOf('{', at);
        var depth = 0;
        var body = string.Empty;
        for (var i = open; i < text.Length; i++)
        {
            if (text[i] == '{') depth++;
            else if (text[i] == '}' && --depth == 0) { body = text[open..(i + 1)]; break; }
        }

        // Unconditional: no mute test guards the Play() call.
        Assert.Contains("foreach (var c in Snapshot()) c.Source.Play();", body, StringComparison.Ordinal);
        Assert.DoesNotContain("Mute", body, StringComparison.Ordinal);

        // …and the summary above it says so. "non-muted" is the exact phrase
        // that was wrong.
        var doc = text[..at];
        var summary = doc[doc.LastIndexOf("/// <summary>", StringComparison.Ordinal)..];
        Assert.DoesNotContain("non-muted", summary, StringComparison.Ordinal);
        Assert.Contains("mute is a GAIN", summary, StringComparison.Ordinal);
    }
}
