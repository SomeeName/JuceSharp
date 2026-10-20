namespace JuceSharp.StemSeparation.Processing;

/// <summary>
/// The checkpoint cannot be read without a full unpickle — which runs code
/// contained in the file — and the caller did not opt in through
/// <see cref="CkptToOnnxConverter.AllowUnsafePickleFallback"/>. A host shows
/// the user the risk and retries with the opt-in only if they accept it.
/// </summary>
public sealed class UnsafeCheckpointException : InvalidOperationException
{
    /// <summary>Create the exception with the converter's message.</summary>
    /// <param name="message">What was refused and why.</param>
    public UnsafeCheckpointException(string message) : base(message)
    {
    }
}
