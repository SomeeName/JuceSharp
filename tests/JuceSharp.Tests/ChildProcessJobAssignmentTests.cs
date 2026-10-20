using System.Diagnostics;
using System.Runtime.InteropServices;
using JuceSharp.StemSeparation.Runtime;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// W4-S9: every registered child has to land in the kill-on-close job, which is
/// the only layer of the cleanup strategy that survives a <c>TerminateProcess</c>
/// from Task Manager — the case where an orphaned python child keeps its
/// multi-GB torch allocation.
/// <para>The RACE the finding describes (a second <c>Register</c> reading the
/// handle while the first thread is still creating it) cannot be reproduced
/// inside a shared test process, because the registry is a process-global that
/// some earlier test has already initialised. What is pinned here is the
/// observable contract the race broke — a registered child IS in the job —
/// which is exactly what the reverted fix fails.</para>
/// </summary>
[Collection("StemSeparatorConcurrency")]
public sealed partial class ChildProcessJobAssignmentTests
{
    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool IsProcessInJob(IntPtr process, IntPtr job,
                                               [MarshalAs(UnmanagedType.Bool)] out bool result);

    [Fact]
    public void A_registered_child_is_inside_the_kill_on_close_job()
    {
        Assert.SkipWhen(!OperatingSystem.IsWindows(), "The job object is the Windows cleanup layer.");

        var job = ChildProcessRegistry.JobHandleForTests;
        Assert.SkipWhen(job == IntPtr.Zero,
            "No job object on this host (nested-job policy or a debugger job); the managed hooks are the fallback.");

        using var child = Process.Start(new ProcessStartInfo
        {
            FileName = "cmd.exe",
            Arguments = "/c ping 127.0.0.1 -n 20 > nul",
            UseShellExecute = false,
            CreateNoWindow = true,
        })!;

        try
        {
            ChildProcessRegistry.Register(child);
            Assert.True(ChildProcessRegistry.IsTracked(child.Id));

            Assert.True(IsProcessInJob(child.Handle, job, out var inJob),
                        $"IsProcessInJob failed (win32 {Marshal.GetLastWin32Error()}).");
            Assert.True(inJob, "the registered child was left OUT of the kill-on-close job.");
        }
        finally
        {
            try { child.Kill(entireProcessTree: true); }
            catch (Exception ex) { Debug.WriteLine($"[test cleanup] {ex.Message}"); }
        }
    }

    /// <summary>
    /// Concurrent registration — the shape the finding is about. It cannot
    /// prove the fix on its own (see the class remarks), but a job that a
    /// PARALLEL burst leaves a child out of would fail here too.
    /// </summary>
    [Fact]
    public void Children_registered_from_many_threads_are_all_in_the_job()
    {
        Assert.SkipWhen(!OperatingSystem.IsWindows(), "The job object is the Windows cleanup layer.");
        var job = ChildProcessRegistry.JobHandleForTests;
        Assert.SkipWhen(job == IntPtr.Zero, "No job object on this host.");

        var children = new Process[6];
        try
        {
            Parallel.For(0, children.Length, i =>
            {
                children[i] = Process.Start(new ProcessStartInfo
                {
                    FileName = "cmd.exe",
                    Arguments = "/c ping 127.0.0.1 -n 20 > nul",
                    UseShellExecute = false,
                    CreateNoWindow = true,
                })!;
                ChildProcessRegistry.Register(children[i]);
            });

            foreach (var c in children)
            {
                Assert.True(IsProcessInJob(c.Handle, job, out var inJob));
                Assert.True(inJob, $"pid {c.Id} was left out of the job.");
            }
        }
        finally
        {
            foreach (var c in children)
            {
                try { c?.Kill(entireProcessTree: true); }
                catch (Exception ex) { Debug.WriteLine($"[test cleanup] {ex.Message}"); }
                c?.Dispose();
            }
        }
    }
}
