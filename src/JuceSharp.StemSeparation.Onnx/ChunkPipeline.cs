namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// Runs a chunked inference as two stages over two buffer slots: <c>run(k, slot)</c> (fill,
/// forward STFT, the model) on the caller's thread, and <c>post(k, slot)</c> (unpack,
/// inverse STFT, overlap-add) on a pool thread WHILE <c>run(k + 1)</c> executes.
/// <para>
/// <b>Why.</b> <c>InferenceSession.Run</c> blocks its thread until the GPU is done, so a
/// sequential loop leaves the GPU idle for the whole host-side inverse of every chunk
/// (twelve iSTFTs and the overlap-add for BS-RoFormer SW). Overlapping them hides that.
/// </para>
/// <para>
/// <b>The slot rule.</b> Chunk <c>k</c> uses slot <c>k % 2</c>. <c>post(k - 1)</c> is waited
/// for AFTER <c>run(k)</c> and BEFORE <c>run(k + 1)</c>, which reuses its slot, so a slot is
/// never written by <c>run</c> while <c>post</c> reads it; posts never overlap one another,
/// so state shared between posts needs no lock.
/// </para>
/// <para>
/// <b>Nothing outlives the call.</b> Whatever ends the loop — a fault in either stage or a
/// cancellation — the pending post is drained before the exception leaves, so no pool
/// thread is still writing into the caller's buffers afterwards. The first failure wins.
/// </para>
/// </summary>
internal static class ChunkPipeline
{
    /// <summary>Runs <paramref name="count"/> chunks.</summary>
    /// <param name="count">Chunks.</param>
    /// <param name="run">The model stage for (chunk, slot), on this thread.</param>
    /// <param name="post">The host stage for (chunk, slot), on a pool thread.</param>
    /// <param name="ct">Checked before each chunk.</param>
    public static void Run(int count, Action<int, int> run, Action<int, int> post, CancellationToken ct)
    {
        ArgumentNullException.ThrowIfNull(run);
        ArgumentNullException.ThrowIfNull(post);
        Task? pending = null;
        try
        {
            for (var k = 0; k < count; k++)
            {
                ct.ThrowIfCancellationRequested();
                run(k, k & 1);
                var previous = pending;
                pending = null;
                previous?.GetAwaiter().GetResult();
                var chunk = k;
                pending = Task.Run(() => post(chunk, chunk & 1), CancellationToken.None);
            }

            var last = pending;
            pending = null;
            last?.GetAwaiter().GetResult();
        }
        finally
        {
            if (pending != null) Drain(pending);
        }
    }

    private static void Drain(Task pending)
    {
        try
        {
            pending.Wait();
        }
        catch (AggregateException)
        {
            // The exception already leaving Run is the one the caller sees; this wait only
            // guarantees the post has stopped touching the buffers.
        }
    }
}
