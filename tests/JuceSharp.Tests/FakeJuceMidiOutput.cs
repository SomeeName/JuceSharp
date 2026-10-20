#nullable disable
using JuceSharp;

namespace JuceSharp.Tests;

/// <summary>
///     A hand-written test double over <see cref="IJuceMidiOutput" />. It exists
///     because <see cref="IJuceAudioEngine.OpenMidiOutput" /> used to return the
///     sealed, internally-constructed <see cref="JuceMidiOutput" />: the fake
///     engine could only answer <c>default!</c>, so any host code that opened a
///     MIDI output got a <see cref="NullReferenceException" /> on first use
///     through the one seam that is supposed to make it testable.
/// </summary>
public sealed class FakeJuceMidiOutput : IJuceMidiOutput
{
    private readonly List<string> _calls = new();

    /// <summary>Every member name this fake was asked for, in order.</summary>
    public IReadOnlyList<string> Calls => _calls;

    /// <summary>Every message handed to <see cref="Send(ReadOnlySpan{byte})" /> or
    /// <see cref="SendAt(ReadOnlySpan{byte},long)" />, with the frame it was
    /// scheduled for (<c>-1</c> for an unscheduled send).</summary>
    public List<(byte[] Data, long RenderFrame)> Messages { get; } = new();

    /// <inheritdoc />
    public int DeviceIndex { get; set; }

    /// <inheritdoc />
    public bool IsDisposed { get; private set; }

    /// <inheritdoc />
    public long SentCount => Messages.Count;

    /// <inheritdoc />
    public long DroppedCount { get; set; }

    /// <inheritdoc />
    public long FailedCount { get; set; }

    /// <inheritdoc />
    public (long Sent, long Dropped) ReadCounters()
    {
        _calls.Add(nameof(ReadCounters));
        return (SentCount, DroppedCount);
    }

    /// <inheritdoc />
    public (long Sent, long Dropped, long Failed) ReadAllCounters()
    {
        _calls.Add(nameof(ReadAllCounters));
        return (SentCount, DroppedCount, FailedCount);
    }

    /// <inheritdoc />
    public void Send(byte status, byte data1, byte data2)
        => Send(stackalloc byte[] { status, data1, data2 });

    /// <inheritdoc />
    public void Send(ReadOnlySpan<byte> data)
    {
        _calls.Add(nameof(Send));
        Messages.Add((data.ToArray(), -1));
    }

    /// <inheritdoc />
    public void SendAt(byte status, byte data1, byte data2, long renderFrame)
        => SendAt(stackalloc byte[] { status, data1, data2 }, renderFrame);

    /// <inheritdoc />
    public void SendAt(ReadOnlySpan<byte> data, long renderFrame)
    {
        _calls.Add(nameof(SendAt));
        Messages.Add((data.ToArray(), renderFrame));
    }

    /// <inheritdoc />
    public void Dispose()
    {
        _calls.Add(nameof(Dispose));
        IsDisposed = true;
    }
}
