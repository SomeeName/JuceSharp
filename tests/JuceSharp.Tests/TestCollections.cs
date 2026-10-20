using Xunit;

namespace JuceSharp.Tests;

// Collection definitions. xunit's unit of parallelism is the COLLECTION, and a
// class with no [Collection] gets an implicit collection of its OWN — so "no
// attribute" does not mean "runs alone", it means "runs alongside everything".
//
// DisableParallelization = true is the only setting that means what this suite
// needs. xunit's own doc for it reads "determines whether tests in this
// collection runs in parallel with any other tests", so a collection carrying
// it is exclusive against the WHOLE run, not merely against its own members.
// That is load-bearing for every rule below, and
// TestCollectionLintTests.Every_sanctioned_collection_disables_parallelization
// pins it: drop the flag from any one definition and the guarantees the other
// definitions rest on go with it.
//
// The three pre-existing definitions live next to their subject and are not
// repeated here: "NullDevice" (NullDeviceTests.cs), "StemSeparatorConcurrency"
// (StemSeparatorConcurrencyTests.cs) and "StemDownloads"
// (CoverageGaps/Wave2StemFixTests.cs).

/// <summary>
/// <b>Anything that reaches the native audio engine.</b>
///
/// <para>
/// A bare <c>new JuceAudioEngine()</c> takes the DEFAULT config, which opens
/// the host's REAL default output device — only <c>NullDevice.CreateEngine()</c>
/// asks for the null type. xunit constructs one class instance per test, so a
/// class with an engine in its constructor performs a full device open and
/// close for every single <c>[NativeAvailableFact]</c> in it. ASIO is
/// EXCLUSIVE; a WASAPI open costs hundreds of milliseconds. Thirty-five such
/// classes carried no attribute at all, which is what "the engine-creating
/// tests time out" meant.
/// </para>
/// <para>
/// Three further kinds of class belong here, because the cure is the same —
/// run alone:
/// </para>
/// <list type="bullet">
/// <item><description>a class that spawns a NATIVE CHILD PROCESS of the engine
/// (<c>juce_vst_host.exe</c>). The stem-separation children have their own
/// collection; this is the other one.</description></item>
/// <item><description>a class that loads the real ONNX runtime — hundreds of MB
/// and every core it can get.</description></item>
/// <item><description>a class that parks MORE THAN ONE thread on a bounded
/// wait. The .NET pool injects roughly one thread per half second above the
/// core count, so a test that blocks its xunit slot waiting on a queued work
/// item can genuinely fail to get a thread inside its bound while the rest of
/// the suite holds every slot — and it then fails with a message that names
/// the symptom ("inference never started") and hides the cause.</description></item>
/// <item><description>a class that reads a PROCESS-WIDE list the engine mutates —
/// <c>Process.GetCurrentProcess().Modules</c> grows whenever any other thread
/// does a <c>LoadLibrary</c>, and a before/after delta answers the sequential
/// hazard but not the concurrent one.</description></item>
/// </list>
/// </summary>
[CollectionDefinition("NativeEngine", DisableParallelization = true)]
public sealed class NativeEngineCollection;

/// <summary>
/// <b>Tests whose assertion is a number read off the garbage collector.</b>
///
/// <para>
/// <c>GC.GetAllocatedBytesForCurrentThread()</c> is
/// <c>alloc_bytes + alloc_bytes_uoh - (alloc_limit - alloc_ptr)</c>. A GC
/// RETIRES the thread's allocation context, so the unconsumed remainder of the
/// current quantum (up to ~8 KB) stops being subtracted and is charged to the
/// thread permanently: <b>two reads straddling a GC differ even when nothing
/// allocated in between.</b> That is not a property of the code under test, it
/// is a property of the instrument, and it is why these probes read zero on an
/// idle box and non-zero under a full parallel gate with stem separation and
/// ONNX large-object traffic alongside.
/// </para>
/// <para>
/// Running alone removes the other threads' GC pressure, which is most of the
/// exposure. It does not remove the mechanism — this thread can still trigger
/// its own collection — so a probe ALSO warms the path up outside the
/// measurement window and retries once. Neither half is sufficient alone and
/// neither is a wall-clock assertion.
/// </para>
/// <para>
/// <c>GC.TryStartNoGCRegion</c> is deliberately NOT used: it THROWS when the
/// budget cannot be granted, which on a loaded box is the same flake wearing a
/// different exception.
/// </para>
/// </summary>
[CollectionDefinition("AllocationProbes", DisableParallelization = true)]
public sealed class AllocationProbesCollection;
