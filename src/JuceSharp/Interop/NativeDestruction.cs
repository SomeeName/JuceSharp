namespace JuceSharp.Interop;

internal interface INativeChild
{
    NativeDestruction Destruction { get; }
}

/// <summary>Completion of native destruction, distinct from claiming a handle.
/// The engine retains this token even after the child wrapper is collected.</summary>
internal sealed class NativeDestruction
{
    private static readonly System.Collections.Concurrent.ConcurrentBag<object> FailedCallbackOwners = new();
    private readonly object _gate = new();
    private int _state; // 0 unclaimed, 1 destroying, 2 destroyed, 3 leaked/failed

    internal bool IsComplete { get { lock (_gate) return _state == 2; } }

    internal void Run(Func<bool> destroy, object? callbackOwner = null)
    {
        lock (_gate)
        {
            if (_state != 0) return;
            _state = 1;
        }
        var success = false;
        try { success = destroy(); }
        finally
        {
            // A leaked native object may still call its registered delegate.
            if (!success && callbackOwner is not null) FailedCallbackOwners.Add(callbackOwner);
            lock (_gate)
            {
                _state = success ? 2 : 3;
                Monitor.PulseAll(_gate);
            }
        }
    }

    internal bool Wait(int timeoutMs = NativeHandleOwner.DefaultDrainTimeoutMs)
    {
        lock (_gate)
        {
            var deadline = Environment.TickCount64 + timeoutMs;
            while (_state < 2)
            {
                var remaining = deadline - Environment.TickCount64;
                if (remaining <= 0) return false;
                Monitor.Wait(_gate, (int)Math.Min(remaining, 25));
            }
            return _state == 2;
        }
    }
}
