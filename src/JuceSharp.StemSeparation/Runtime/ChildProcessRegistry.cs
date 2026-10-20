// ReSharper disable InconsistentNaming -- private static fields use _camelCase consistent with project style.
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace JuceSharp.StemSeparation.Runtime;

/// <summary>
/// Tracks every child <see cref="Process"/> the JuceSharp pipeline spawns
/// and guarantees they are torn down when the managed host dies —
/// regardless of whether the host exits cleanly, throws an unhandled
/// exception, gets Ctrl-C'd, or is terminated by the OS.
///
/// <para>The cleanup strategy is layered, strongest-first:</para>
/// <list type="number">
///   <item>
///     <b>Windows Job Object with <c>JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE</c>.</b>
///     We create exactly one unnamed job object at startup and assign
///     every child python process into it. When the parent .NET process
///     dies for <i>any</i> reason (including <c>TerminateProcess</c>
///     from Task Manager), the kernel closes the job handle, which
///     atomically terminates every member — no managed handler needed.
///   </item>
///   <item>
///     <b>Managed lifecycle hooks.</b> On platforms without job objects
///     (or as belt-and-braces on Windows) we hook
///     <c>AppDomain.ProcessExit</c>, <c>AssemblyLoadContext.Unloading</c>,
///     <c>AppDomain.UnhandledException</c>, and <c>Console.CancelKeyPress</c>
///     and walk the live-process registry, calling
///     <c>Kill(entireProcessTree: true)</c> on each.
///   </item>
///   <item>
///     <b>POSIX <c>prctl(PR_SET_PDEATHSIG, SIGKILL)</c>.</b> On Linux we
///     ask the kernel to SIGKILL the child the moment the parent dies.
///     On macOS, where prctl is unavailable, we rely on the managed
///     hooks above.
///   </item>
/// </list>
/// <para><b>The registry NEVER owns the <see cref="Process"/> objects it
/// tracks.</b> It only kills and untracks them; disposal belongs to whoever
/// called <c>Process.Start</c>. See <c>Untrack</c> for what happened when it
/// did dispose them.</para>
/// <para><b>Inside DotCharter this job object is a redundant second layer.</b>
/// The app puts ITSELF into a kill-on-close job
/// (<c>DotCharter.Services/Infrastructure/Process/ChildProcessJob</c>) and
/// Windows children inherit it, so a python child is already covered before
/// <see cref="Register"/> runs; nesting (Win8+) just makes ours a child job
/// that fires at the same moment. It is kept because JuceSharp is a reusable
/// submodule and a standalone host has no outer job. Note ours does NOT set
/// <c>JOB_OBJECT_LIMIT_BREAKAWAY_OK</c> while the app's job does — breakaway is
/// governed by the IMMEDIATE job, so a descendant of a python child that asked
/// for <c>CREATE_BREAKAWAY_FROM_JOB</c> would be refused. Nothing does today.</para>
/// </summary>
internal static class ChildProcessRegistry
{
    private static readonly ConcurrentDictionary<int, Process> _live = new();
    private static int _initialized;

    /// <summary>
    /// The kill-on-close job, created at most once.
    /// <para><b>Lazy, not a field written inside <c>EnsureHooks</c>.</b>
    /// <c>_initialized</c> was flipped BEFORE the handle was assigned and the
    /// field was not volatile, so a thread calling <see cref="Register"/> while
    /// another was inside <c>CreateKillOnCloseJob</c> returned from
    /// <c>EnsureHooks</c> immediately, read <c>IntPtr.Zero</c>, and left ITS
    /// child out of the job — silently disarming the strongest layer of the
    /// cleanup strategy for that child alone, which is the one that survives a
    /// <c>TerminateProcess</c> from Task Manager. Two concurrent separations, or
    /// a separation racing a <c>CkptToOnnxConverter</c>, is a configuration
    /// <c>PythonEnvManager</c> documents as normal.
    /// <c>ExecutionAndPublication</c> makes the second caller WAIT for the
    /// handle instead of seeing a half-initialised one (W4-S9).</para>
    /// </summary>
    private static readonly Lazy<IntPtr> JobHandle = new(static () =>
    {
        if (!OperatingSystem.IsWindows()) return IntPtr.Zero;
        try { return CreateKillOnCloseJob(); }
        catch { return IntPtr.Zero; /* fall back to the managed hooks */ }
    }, LazyThreadSafetyMode.ExecutionAndPublication);

    /// <summary>Test hook: the job every registered child should be inside, or
    /// <see cref="IntPtr.Zero"/> where there is none.</summary>
    internal static IntPtr JobHandleForTests => JobHandle.Value;

    /// <summary>Number of child processes currently registered. Used by tests.</summary>
    public static int LiveCount => _live.Count;

    public static void Register(Process p)
    {
        EnsureHooks();
        try
        {
            if (p.HasExited) return;
        }
        catch { /* HasExited can throw if already disposed; treat as exited */ return; }

        // Capture the pid ONCE. Reading p.Id after the OWNER has disposed the
        // Process throws, which would strand the entry in _live forever.
        var pid = p.Id;
        _live[pid] = p;
        try
        {
            // Subscribe BEFORE arming the event. Setting EnableRaisingEvents on
            // an already-exited process raises Exited immediately, so the other
            // order can drop the notification and leak the entry forever.
            p.Exited += (_, _) => Untrack(pid, p);
            p.EnableRaisingEvents = true;
        }
        catch { /* race with process exit; the HasExited re-check below covers it */ }

        // The process may have exited between the HasExited check above and the
        // subscription, in which case Exited never fires for us.
        try { if (p.HasExited) Untrack(pid, p); }
        catch { Untrack(pid, p); }

        var job = JobHandle.Value;
        if (OperatingSystem.IsWindows() && job != IntPtr.Zero)
        {
            // AssignProcessToJobObject returns BOOL — it does NOT throw — so the
            // try/catch alone would let a refused assignment (nested-job policy,
            // a debugger job, pre-Win8) silently disarm the strongest layer of
            // the cleanup strategy documented above.
            try
            {
                if (!AssignProcessToJobObject(job, p.Handle))
                    Debug.WriteLine($"ChildProcessRegistry: AssignProcessToJobObject failed (win32 {Marshal.GetLastWin32Error()}); falling back to the managed hooks.");
            }
            catch (Exception ex) { Debug.WriteLine($"ChildProcessRegistry: job assignment threw: {ex.Message}"); }
        }
    }

    /// <summary>
    /// Stop tracking <paramref name="p"/> — but ONLY if it is still the process
    /// registered under its pid.
    /// <para>This deliberately does NOT dispose: the registry never owns the
    /// <see cref="Process"/>. Its single production caller
    /// (<c>PythonEnvManager.RunStreamingCore</c>) holds it in a <c>using</c> and
    /// reads <c>p.ExitCode</c> after <c>WaitForExitAsync</c> returns. Disposing
    /// here ran FIRST (this handler is subscribed before the one
    /// <c>WaitForExitAsync</c> installs), so the owner's read then threw
    /// <see cref="InvalidOperationException"/> "No process is associated with
    /// this object" — turning every SUCCESSFUL separation into a reported
    /// failure, and making a short-lived probe (the <c>pip --version</c> check)
    /// fail so hard that pip was re-bootstrapped on every call.</para>
    /// <para>Pids are recycled, so a stale exit must not evict the process that
    /// currently owns the pid — hence the value-matching overload of
    /// <c>TryRemove</c> rather than a bare key removal.</para>
    /// </summary>
    private static void Untrack(int pid, Process p) => _live.TryRemove(new KeyValuePair<int, Process>(pid, p));

    /// <summary>Kill every still-live registered child. Best-effort and safe to call repeatedly.</summary>
    public static void KillAll()
    {
        foreach (var kv in _live.ToArray())
        {
            try { if (!kv.Value.HasExited) kv.Value.Kill(entireProcessTree: true); }
            catch { /* best effort */ }
            // Remove only — see Untrack: the caller owns the Process object.
            _live.TryRemove(kv);
        }
    }

    /// <summary>Test hook: is <paramref name="pid"/> currently tracked?</summary>
    internal static bool IsTracked(int pid) => _live.ContainsKey(pid);

    private static void EnsureHooks()
    {
        if (Interlocked.Exchange(ref _initialized, 1) != 0) return;
        AppDomain.CurrentDomain.ProcessExit += static (_, _) => KillAll();
        AppDomain.CurrentDomain.UnhandledException += static (_, _) => KillAll();
        try { AssemblyLoadContext.Default.Unloading += static _ => KillAll(); }
        catch (Exception ex) { Debug.WriteLine($"ChildProcessRegistry: AssemblyLoadContext hook failed: {ex.Message}"); }
        try { Console.CancelKeyPress += static (_, _) => { KillAll(); /* let default handler run */ }; }
        catch (Exception ex) { Debug.WriteLine($"ChildProcessRegistry: Console.CancelKeyPress hook failed: {ex.Message}"); }
    }

    // --- Win32 Job Object plumbing -----------------------------------

    [Flags]
    private enum JobObjectLimit : uint
    {
        KillOnJobClose = 0x00002000,
    }

    // ReSharper disable InconsistentNaming -- struct names match Win32 API conventions for clarity when cross-referenced with MSDN.
    [StructLayout(LayoutKind.Sequential)]
    private struct IO_COUNTERS
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public JobObjectLimit LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }
    // ReSharper restore InconsistentNaming

    private const int JobObjectExtendedLimitInformation = 9;

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObject(IntPtr lpJobAttributes, string? lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetInformationJobObject(IntPtr hJob, int infoClass, IntPtr lpJobObjectInfo, uint cbJobObjectInfoLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool AssignProcessToJobObject(IntPtr hJob, IntPtr hProcess);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);

    private static IntPtr CreateKillOnCloseJob()
    {
        if (!OperatingSystem.IsWindows()) return IntPtr.Zero;
        var h = CreateJobObject(IntPtr.Zero, null);
        if (h == IntPtr.Zero) return IntPtr.Zero;

        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            BasicLimitInformation = new JOBOBJECT_BASIC_LIMIT_INFORMATION
            {
                LimitFlags = JobObjectLimit.KillOnJobClose,
            },
        };
        var size = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
        var ptr = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(info, ptr, false);
            if (!SetInformationJobObject(h, JobObjectExtendedLimitInformation, ptr, (uint)size))
            {
                // Without KILL_ON_JOB_CLOSE the job is useless AND would still
                // hold every member for the process lifetime — close it rather
                // than leaking a limitless job object.
                CloseHandle(h);
                return IntPtr.Zero;
            }
        }
        finally { Marshal.FreeHGlobal(ptr); }
        return h;
    }
}
