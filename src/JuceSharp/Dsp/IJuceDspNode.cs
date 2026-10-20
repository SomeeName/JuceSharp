namespace JuceSharp.Dsp;

/// <summary>
/// The contract of <see cref="JuceDspNode" />; implemented by
/// <see cref="JuceDspNode" />. A test seam for per-pedal UI and preset code —
/// see <c>docs/guides/testing-without-native.md</c>.
/// <para>
///     Every member is <c>&lt;inheritdoc&gt;</c> onto its concrete counterpart:
///     one source of truth. Pinned by <c>Wave2DocsInterfaceDocTests</c>.
/// </para>
/// </summary>
public interface IJuceDspNode
{
    /// <inheritdoc cref="JuceDspNode.Id" />
    int Id { get; }

    /// <inheritdoc cref="JuceDspNode.TypeId" />
    string TypeId { get; }

    /// <inheritdoc cref="JuceDspNode.Player" />
    int Player { get; }

    /// <inheritdoc cref="JuceDspNode.Bypass" />
    bool Bypass { get; set; }

    /// <inheritdoc cref="JuceDspNode.Branch" />
    int Branch { get; set; }

    /// <inheritdoc cref="JuceDspNode.LatencySamples" />
    int LatencySamples { get; }

    /// <inheritdoc cref="JuceDspNode.Meter" />
    JuceDspNodeMeter Meter { get; }

    /// <inheritdoc cref="JuceDspNode.GetParam(string)" />
    float GetParam(string paramId);

    /// <inheritdoc cref="JuceDspNode.SetParam(string, float)" />
    void SetParam(string paramId, float value);

    /// <inheritdoc cref="JuceDspNode.GetString(string)" />
    string GetString(string paramId);

    /// <inheritdoc cref="JuceDspNode.SetString(string, string)" />
    void SetString(string paramId, string? value);

    /// <inheritdoc cref="JuceDspNode.Remove" />
    void Remove();

    /// <inheritdoc cref="JuceDspNode.MoveTo(int)" />
    void MoveTo(int newIndex);

    /// <summary>The signal this node keys from (compressor / gate); rejected on a
    /// module that does not advertise sidechain support. See
    /// <see cref="JuceDspNode.Sidechain" />.</summary>
    JuceDspSidechainSource Sidechain { get; set; }
}
