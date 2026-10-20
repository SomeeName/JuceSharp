using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace JuceSharp.Tests.CoverageGaps;

/// <summary>How a <see cref="LoopbackFileServer"/> should behave.</summary>
internal sealed record LoopbackServerOptions
{
    /// <summary>Request path the URL is built for. The server serves the same
    /// body whatever is asked for; only the framing has to be valid.</summary>
    public string Path { get; init; } = "/payload.bin";

    /// <summary>
    /// When true the server advertises <c>Accept-Ranges: bytes</c> and honours
    /// a <c>Range: bytes=N-</c> request with a <c>206 Partial Content</c>.
    /// When false it IGNORES the header and answers <c>200 OK</c> with the
    /// whole body — which is the case a resuming client must detect and
    /// restart from, because appending a whole body to a prefix silently
    /// corrupts the file.
    /// </summary>
    public bool SupportsRange { get; init; }

    /// <summary>
    /// When set, the server writes this many bytes of the body and then STOPS,
    /// holding the connection open until <see cref="LoopbackFileServer.ReleaseStall"/>
    /// or disposal. It is how a mid-transfer cancellation is made deterministic
    /// without a clock.
    /// </summary>
    public int? StallAfterBytes { get; init; }

    /// <summary>
    /// When true, any request carrying a <c>Range</c> header is answered
    /// <c>416 Requested Range Not Satisfiable</c> — the case where our partial
    /// is already at or past the resource. A request WITHOUT a Range header is
    /// served normally, so a client that correctly restarts without resuming
    /// completes, and one that re-sends the same range loops for ever.
    /// </summary>
    public bool RangeNotSatisfiable { get; init; }

    /// <summary>
    /// When &gt; 0, the server DECLARES this many bytes more than it sends and
    /// then closes the connection cleanly. That is what a truncating proxy or
    /// a flaky mirror produces, and it is invisible to the read loop: the read
    /// returns 0 and everything downstream succeeds.
    /// </summary>
    public int OverstateContentLengthBy { get; init; }

    /// <summary>
    /// When true the response carries NO <c>Content-Length</c> and is
    /// close-delimited. That is the one truncation shape no HTTP client can
    /// detect — EOF *is* the terminator — and therefore the case
    /// <c>OnnxModelEntry.SizeBytes</c> exists for. Pair it with
    /// <see cref="TruncateBodyTo"/>.
    /// </summary>
    public bool OmitContentLength { get; init; }

    /// <summary>When set, only this many bytes of the body are sent before the
    /// connection is closed cleanly.</summary>
    public int? TruncateBodyTo { get; init; }

    /// <summary>
    /// When true, a request carrying a <c>Range</c> header is answered
    /// <c>206 Partial Content</c> with <c>Content-Range: bytes 0-N/N</c> and the
    /// WHOLE body — what a caching proxy that normalises or clamps the requested
    /// range does. The status code alone says "a range was honoured"; only
    /// <c>Content-Range</c> says WHICH, so a client that trusts the status code
    /// appends a whole body to its prefix and doubles the file.
    /// </summary>
    public bool ClampRangeToWholeBody { get; init; }
}

/// <summary>
/// A one-shot-per-connection HTTP/1.1 responder on 127.0.0.1 that serves a
/// fixed byte payload, so the download path can be exercised end to end
/// without the network and without ever fetching the real 10 MB Python archive
/// or a 300 MB model.
///
/// <para>
/// A raw <see cref="TcpListener"/> rather than <see cref="HttpListener"/> on
/// purpose: <c>HttpListener</c> needs a URL ACL (or elevation) to reserve a
/// prefix, which is not something a test may assume. Loopback TCP needs
/// neither, and is not subject to the host firewall.
/// </para>
/// <para>Bound to port 0 — the OS picks a free ephemeral port.</para>
/// </summary>
internal sealed class LoopbackFileServer : IDisposable
{
    private readonly TcpListener _listener;
    private readonly CancellationTokenSource _cts = new();
    private readonly Task _loop;
    private readonly ManualResetEventSlim _stall = new(false);
    private readonly List<string?> _rangeHeaders = [];
    private readonly object _gate = new();

    private LoopbackFileServer(TcpListener listener, byte[] payload, LoopbackServerOptions options)
    {
        _listener = listener;
        Url = $"http://127.0.0.1:{((IPEndPoint)listener.LocalEndpoint).Port}{options.Path}";
        _loop = Task.Run(() => ServeAsync(payload, options, _cts.Token));
    }

    public string Url { get; }

    /// <summary>The <c>Range</c> header of every request received, in order;
    /// null for a request that carried none. Lets a test assert that a resume
    /// actually ASKED to resume rather than silently re-fetching.</summary>
    public IReadOnlyList<string?> ReceivedRangeHeaders
    {
        get { lock (_gate) return _rangeHeaders.ToArray(); }
    }

    /// <summary>Lets a stalled response finish.</summary>
    public void ReleaseStall() => _stall.Set();

    public static LoopbackFileServer Start(byte[] payload, string path = "/payload.bin") =>
        Start(payload, new LoopbackServerOptions { Path = path });

    public static LoopbackFileServer Start(byte[] payload, LoopbackServerOptions options)
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        return new LoopbackFileServer(listener, payload, options);
    }

    private async Task ServeAsync(byte[] payload, LoopbackServerOptions options, CancellationToken ct)
    {
        try
        {
            while (!ct.IsCancellationRequested)
            {
                using var client = await _listener.AcceptTcpClientAsync(ct).ConfigureAwait(false);
                using var stream = client.GetStream();

                var head = await ReadHeadAsync(stream, ct).ConfigureAwait(false);
                var range = HeaderValue(head, "Range");
                lock (_gate) _rangeHeaders.Add(range);

                if (options.RangeNotSatisfiable && range is not null)
                {
                    var refusal = "HTTP/1.1 416 Requested Range Not Satisfiable\r\n" +
                                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
                    await stream.WriteAsync(Encoding.ASCII.GetBytes(refusal), ct).ConfigureAwait(false);
                    await stream.FlushAsync(ct).ConfigureAwait(false);
                    client.Client.Shutdown(SocketShutdown.Send);
                    continue;
                }

                if (options.ClampRangeToWholeBody && range is not null)
                {
                    var clamped = new StringBuilder();
                    clamped.Append("HTTP/1.1 206 Partial Content\r\n");
                    clamped.Append("Content-Type: application/octet-stream\r\n");
                    clamped.Append("Accept-Ranges: bytes\r\n");
                    clamped.Append(CultureInfo.InvariantCulture,
                        $"Content-Range: bytes 0-{payload.Length - 1}/{payload.Length}\r\n");
                    clamped.Append(CultureInfo.InvariantCulture, $"Content-Length: {payload.Length}\r\n");
                    clamped.Append("Connection: close\r\n\r\n");
                    await stream.WriteAsync(Encoding.ASCII.GetBytes(clamped.ToString()), ct).ConfigureAwait(false);
                    await stream.WriteAsync(payload, ct).ConfigureAwait(false);
                    await stream.FlushAsync(ct).ConfigureAwait(false);
                    client.Client.Shutdown(SocketShutdown.Send);
                    continue;
                }

                var from = options.SupportsRange ? ParseRangeStart(range) : 0;
                if (from > payload.Length) from = payload.Length;
                var body = from == 0 ? payload : payload[from..];

                var sb = new StringBuilder();
                sb.Append(from > 0 ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n");
                sb.Append("Content-Type: application/octet-stream\r\n");
                if (options.SupportsRange) sb.Append("Accept-Ranges: bytes\r\n");
                if (from > 0)
                    sb.Append(CultureInfo.InvariantCulture,
                        $"Content-Range: bytes {from}-{payload.Length - 1}/{payload.Length}\r\n");
                if (!options.OmitContentLength)
                    sb.Append(CultureInfo.InvariantCulture,
                        $"Content-Length: {body.Length + options.OverstateContentLengthBy}\r\n");
                sb.Append("Connection: close\r\n\r\n");

                await stream.WriteAsync(Encoding.ASCII.GetBytes(sb.ToString()), ct).ConfigureAwait(false);

                if (options.TruncateBodyTo is { } keep && keep < body.Length)
                {
                    await stream.WriteAsync(body.AsMemory(0, keep), ct).ConfigureAwait(false);
                    await stream.FlushAsync(ct).ConfigureAwait(false);
                    client.Client.Shutdown(SocketShutdown.Send);
                    continue;
                }

                if (options.StallAfterBytes is { } stallAt && stallAt < body.Length)
                {
                    await stream.WriteAsync(body.AsMemory(0, stallAt), ct).ConfigureAwait(false);
                    await stream.FlushAsync(ct).ConfigureAwait(false);
                    // Hold the response open. The client is expected to give up
                    // (cancellation) while we sit here.
                    _stall.Wait(ct);
                    await stream.WriteAsync(body.AsMemory(stallAt), ct).ConfigureAwait(false);
                }
                else
                {
                    await stream.WriteAsync(body, ct).ConfigureAwait(false);
                }

                await stream.FlushAsync(ct).ConfigureAwait(false);
                client.Client.Shutdown(SocketShutdown.Send);
            }
        }
        catch (OperationCanceledException) { /* shutting down */ }
        catch (ObjectDisposedException) { /* shutting down */ }
        catch (SocketException) { /* client went away */ }
        catch (IOException) { /* client went away */ }
    }

    private static async Task<string> ReadHeadAsync(NetworkStream stream, CancellationToken ct)
    {
        var buf = new byte[4096];
        var head = new StringBuilder();
        while (!head.ToString().Contains("\r\n\r\n", StringComparison.Ordinal))
        {
            var n = await stream.ReadAsync(buf, ct).ConfigureAwait(false);
            if (n <= 0) break;
            head.Append(Encoding.ASCII.GetString(buf, 0, n));
        }
        return head.ToString();
    }

    private static string? HeaderValue(string head, string name)
    {
        foreach (var line in head.Split("\r\n"))
        {
            if (!line.StartsWith(name + ":", StringComparison.OrdinalIgnoreCase)) continue;
            return line[(name.Length + 1)..].Trim();
        }
        return null;
    }

    /// <summary>Parses the START of a <c>bytes=N-</c> request. Only the
    /// open-ended form a resuming download sends is supported.</summary>
    private static int ParseRangeStart(string? range)
    {
        if (string.IsNullOrWhiteSpace(range)) return 0;
        const string prefix = "bytes=";
        if (!range.StartsWith(prefix, StringComparison.OrdinalIgnoreCase)) return 0;
        var spec = range[prefix.Length..];
        var dash = spec.IndexOf('-');
        if (dash <= 0) return 0;
        return int.TryParse(spec[..dash], NumberStyles.Integer, CultureInfo.InvariantCulture, out var from)
            ? from
            : 0;
    }

    public void Dispose()
    {
        _stall.Set();
        _cts.Cancel();
        try { _listener.Stop(); } catch (Exception) { /* best effort */ }
        try { _loop.Wait(TimeSpan.FromSeconds(5)); } catch (Exception) { /* best effort */ }
        _stall.Dispose();
        _cts.Dispose();
    }
}
