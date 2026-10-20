namespace JuceSharp.Dsp;

// The IJuceDspChain implementation, kept OUT of JuceDspChain.cs so the rig
// surface itself reads unchanged. Everything here is an EXPLICIT interface
// implementation over an existing public member: the interface answers in
// IJuceDspNode terms (so a fake chain can hand back fake nodes) while a caller
// holding the concrete type still gets JuceDspNode, which is why no public
// signature moved.
public sealed partial class JuceDspChain : IJuceDspChain
{
    IReadOnlyList<IJuceDspNode> IJuceDspChain.Nodes => Nodes;

    IJuceDspNode IJuceDspChain.AddNode(string moduleTypeId) => AddNode(moduleTypeId);

    IJuceDspNode IJuceDspChain.AddNodeBypassed(string moduleTypeId) => AddNodeBypassed(moduleTypeId);

    bool IJuceDspChain.TryGetNode(int nodeId, out IJuceDspNode node)
    {
        var found = TryGetNode(nodeId, out var concrete);
        // `concrete` is [MaybeNullWhen(false)]; the interface's out-param is not
        // annotated, so the null is forgiven here rather than laundered away.
        node = concrete!;
        return found;
    }

    // By ID, not by cast: an IJuceDspNode handed to a real chain may be a fake,
    // and the id is the only thing the engine addresses anyway.
    void IJuceDspChain.RemoveNode(IJuceDspNode node)
    {
        ArgumentNullException.ThrowIfNull(node);
        RemoveNode(node.Id);
    }

    void IJuceDspChain.MoveNode(IJuceDspNode node, int newIndex)
    {
        ArgumentNullException.ThrowIfNull(node);
        MoveNode(node.Id, newIndex);
    }
}
