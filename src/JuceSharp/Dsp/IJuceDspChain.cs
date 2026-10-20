using System.Diagnostics.CodeAnalysis;

namespace JuceSharp.Dsp;

/// <summary>
///     The contract of <see cref="JuceDspChain" />; implemented by
///     <see cref="JuceDspChain" />. A test seam for pedalboard / preset code,
///     which is otherwise untestable without the native rig — see
///     <c>docs/guides/testing-without-native.md</c>.
///     <para>
///         Node-valued members answer in <see cref="IJuceDspNode" /> terms so a
///         fake chain can hand back fake nodes; on the concrete class those are
///         EXPLICIT implementations over the existing public ones, so nothing a
///         caller holding a <see cref="JuceDspChain" /> sees has changed.
///     </para>
///     <para>
///         Every member is <c>&lt;inheritdoc&gt;</c> onto its concrete counterpart:
///         one source of truth. Pinned by <c>Wave2DocsInterfaceDocTests</c>.
///     </para>
/// </summary>
public interface IJuceDspChain
{
    /// <inheritdoc cref="JuceDspChain.Player" />
    int Player { get; }

    /// <inheritdoc cref="JuceDspChain.NodeCount" />
    int NodeCount { get; }

    /// <inheritdoc cref="JuceDspChain.LatencySamples" />
    int LatencySamples { get; }

    /// <inheritdoc cref="JuceDspChain.MeteringEnabled" />
    bool MeteringEnabled { get; set; }

    /// <inheritdoc cref="JuceDspChain.Nodes" />
    IReadOnlyList<IJuceDspNode> Nodes { get; }

    /// <inheritdoc cref="JuceDspChain.ToGlobalNodeId(int)" />
    int ToGlobalNodeId(int localNodeId);

    /// <inheritdoc cref="JuceDspChain.AddNode(string)" />
    IJuceDspNode AddNode(string moduleTypeId);

    /// <inheritdoc cref="JuceDspChain.AddNodeBypassed(string)" />
    IJuceDspNode AddNodeBypassed(string moduleTypeId);

    /// <inheritdoc cref="JuceDspChain.TryGetNode(int, out JuceDspNode)" />
    bool TryGetNode(int nodeId, [MaybeNullWhen(false)] out IJuceDspNode node);

    /// <inheritdoc cref="JuceDspChain.RemoveNode(JuceDspNode)" />
    void RemoveNode(IJuceDspNode node);

    /// <inheritdoc cref="JuceDspChain.RemoveNode(int)" />
    void RemoveNode(int nodeId);

    /// <inheritdoc cref="JuceDspChain.MoveNode(JuceDspNode, int)" />
    void MoveNode(IJuceDspNode node, int newIndex);

    /// <inheritdoc cref="JuceDspChain.MoveNode(int, int)" />
    void MoveNode(int nodeId, int newIndex);

    /// <inheritdoc cref="JuceDspChain.Clear" />
    void Clear();

    /// <inheritdoc cref="JuceDspChain.NodeIds" />
    int[] NodeIds();

    /// <inheritdoc cref="JuceDspChain.TryReadMeters(Span{JuceDspNodeMeter})" />
    [Obsolete("Use TryReadMeters(Span<JuceDspNodeMeter>, out int): this overload cannot distinguish 'no nodes' from 'engine gone'.")]
    int TryReadMeters(Span<JuceDspNodeMeter> dest);

    /// <inheritdoc cref="JuceDspChain.TryReadMeters(Span{JuceDspNodeMeter}, out int)" />
    bool TryReadMeters(Span<JuceDspNodeMeter> dest, out int nodeCount);

    /// <inheritdoc cref="JuceDspChain.TryReadMeter(int, out JuceDspNodeMeter)" />
    bool TryReadMeter(int nodeId, out JuceDspNodeMeter meter);

    /// <inheritdoc cref="JuceDspChain.ProcessOffline(ReadOnlySpan{float}, int, int, Span{float})" />
    void ProcessOffline(ReadOnlySpan<float> input, int channels, int sampleRate, Span<float> output);

    /// <inheritdoc cref="JuceDspChain.ProcessOffline(ReadOnlySpan{float}, ReadOnlySpan{float}, int, Span{float}, Span{float})" />
    void ProcessOffline(ReadOnlySpan<float> left, ReadOnlySpan<float> right, int sampleRate,
                        Span<float> outLeft, Span<float> outRight);

    /// <summary>Open an edit scope: structure and parameter changes accumulate
    /// and publish as ONE snapshot on <see cref="JuceDspEditScope.Commit" />;
    /// disposing without a commit aborts. See <see cref="JuceDspChain.BeginEdit" />.</summary>
    JuceDspEditScope BeginEdit();

    /// <summary>Whether an edit scope is open on this seat (false on a disposed engine).</summary>
    bool IsEditing { get; }

    /// <summary>Delay-compensate parallel branches at the merger (default on).
    /// See <see cref="JuceDspChain.BranchLatencyCompensation" />.</summary>
    bool BranchLatencyCompensation { get; set; }

    /// <summary>Ring removed delays/reverbs out as post-chain tails (default off).
    /// See <see cref="JuceDspChain.Spillover" />.</summary>
    bool Spillover { get; set; }

    /// <summary>Tails still ringing on this seat. See <see cref="JuceDspChain.SpillTailCount" />.</summary>
    int SpillTailCount { get; }

    /// <inheritdoc cref="JuceDspChain.ProcessOffline(Span{float}, int, int, ReadOnlySpan{JuceDspAutomationEvent})" />
    void ProcessOffline(Span<float> buffer, int channels, int sampleRate,
                        ReadOnlySpan<JuceDspAutomationEvent> events);
}
