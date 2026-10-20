using JuceSharp.Interop;

namespace JuceSharp;

/// <summary>
/// Thrown by the managed JuceSharp surface whenever a native API call
/// returns a non-<c>Ok</c> status. Carries the typed
/// <see cref="JuceAudioStatus"/> as well as the friendly name of the
/// operation that failed so callers can branch precisely.
/// </summary>
public sealed class JuceAudioException : Exception
{
    internal JuceAudioException(string message, NativeMethods.DcaStatus status, string operation)
        : this(message, (JuceAudioStatus)(int)status, operation) { }

    /// <summary>
    ///     Build one from OUTSIDE the wrapper's own <c>Check</c> path — the shape a
    ///     backend needs when it has a real native status and wants to report it
    ///     rather than flatten it into an <see cref="InvalidOperationException" />
    ///     whose only content is a string. The message is composed for you.
    /// </summary>
    /// <param name="status">The status the native call returned.</param>
    /// <param name="operation">The failed entry point, e.g. <c>"dca_stems_separate_file"</c>.</param>
    public JuceAudioException(JuceAudioStatus status, string operation)
        : this(null, status, operation) { }

    /// <summary>
    ///     <see cref="JuceAudioException(JuceAudioStatus, string)" /> with detail.
    /// </summary>
    /// <param name="message">Detail; a default is composed when null.</param>
    /// <param name="status">The status the native call returned.</param>
    /// <param name="operation">The failed entry point, e.g. <c>"dca_stems_separate_file"</c>.</param>
    /// <remarks>
    ///     <b>The status sits BETWEEN the two strings on purpose.</b> The public
    ///     ctor this replaced was <c>(status, operation, message = null)</c> —
    ///     two adjacent <c>string</c>s, so transposing them compiled cleanly and
    ///     produced an exception whose <see cref="Operation" /> was a sentence
    ///     and whose message was an entry-point name. Nothing could have caught
    ///     that but a reader. Here the parameter order also matches the internal
    ///     <c>Check</c> ctor, so there is one shape to remember.
    /// </remarks>
    public JuceAudioException(string? message, JuceAudioStatus status, string operation)
        : base(message ?? $"{operation} failed with {status}.")
    {
        Status     = status;
        StatusCode = (int)status;
        Operation  = operation;
    }

    /// <summary>Typed status value returned by the native engine.</summary>
    public JuceAudioStatus Status { get; }

    /// <summary>Native status code as an integer (compat with older
    /// callers that switched on the raw int).</summary>
    public int StatusCode { get; }

    /// <summary>Name of the failed operation, e.g. <c>"source_open_file"</c>
    /// or <c>"plugin_load_vst3"</c>. Useful for log correlation.</summary>
    public string Operation { get; }

    /// <summary>The base exception rendering with an extra indented line carrying
    /// <see cref="Operation"/>, <see cref="Status"/> and <see cref="StatusCode"/>, so a
    /// logged stack trace identifies the failed entry point without a separate lookup.</summary>
    public override string ToString()
        => $"{base.ToString()}{Environment.NewLine}    Operation={Operation}, Status={Status} ({StatusCode})";
}

/// <summary>
///     Thrown by <see cref="JuceAudioEngine" />'s constructor when the loaded
///     <c>juce_sharp.dll</c> reports a different <c>DCA_ABI_VERSION</c> than this
///     wrapper was built against. The comparison is EQUALITY, not "at least", so
///     this is the one and only failure mode of a stale DLL — every missing entry
///     point is caught here rather than at the call that would have used it.
///     <b>The fix is always to rebuild the native DLL</b>, never to widen the check.
///     <para>
///         Derives from <see cref="InvalidOperationException" /> deliberately: that
///         is what this used to throw, and a host that already catches it (or logs
///         it) keeps working while gaining the two numbers as data.
///     </para>
/// </summary>
public sealed class JuceAbiMismatchException : InvalidOperationException
{
    internal JuceAbiMismatchException(uint expected, uint actual)
        : base($"JuceSharp.Native ABI mismatch: wrapper expects {expected}, native is {actual}. "
               + "Rebuild the native DLL.")
    {
        ExpectedAbiVersion = expected;
        NativeAbiVersion   = actual;
    }

    /// <summary>The <c>DCA_ABI_VERSION</c> this wrapper build mirrors.</summary>
    public uint ExpectedAbiVersion { get; }

    /// <summary>What the loaded DLL reported from <c>dca_abi_version()</c>.</summary>
    public uint NativeAbiVersion { get; }
}
