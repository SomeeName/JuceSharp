using System.Reflection;
using System.Runtime.CompilerServices;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Every handle-owning wrapper in JuceSharp has a finalizer, and a finalizer
/// that throws is NOT a caught exception — the CLR tears the whole process
/// down. So each one must tolerate the single state it can be handed that no
/// other code path ever sees: a <b>partially-constructed instance</b>, whose
/// fields are all still null/zero because the constructor threw after the CLR
/// had already made the object finalizable.
///
/// <para>This is not theoretical. <c>JuceAudioPlugin</c>'s constructor rejects
/// a zero handle by throwing BEFORE assigning <c>_owner</c>, and its finalizer
/// then dereferenced that null. The throw itself was correct and its unit test
/// passed; the corpse killed the test host minutes later, at a GC-dependent
/// point that moved between runs (392 tests one run, 251 the next) — which
/// reads exactly like a flaky test and is not one.</para>
/// </summary>
public sealed class FinalizerSafetyTests
{
    /// <summary>Every type in the JuceSharp assembly that declares its own
    /// finalizer. Discovered rather than listed, so a new handle wrapper is
    /// covered the day it is written.</summary>
    public static TheoryData<string> FinalizableTypes()
    {
        var data = new TheoryData<string>();
        foreach (var name in FinalizableTypeNames()) data.Add(name);
        return data;
    }

    private static List<string> FinalizableTypeNames()
    {
        var names = new List<string>();
        foreach (var t in typeof(JuceAudioEngine).Assembly.GetTypes())
        {
            if (t.IsAbstract || t.IsInterface || t.ContainsGenericParameters) continue;
            if (FinalizerOf(t) is not null) names.Add(t.FullName!);
        }
        return names;
    }

    private static MethodInfo? FinalizerOf(Type t)
    {
        var m = t.GetMethod("Finalize", BindingFlags.NonPublic | BindingFlags.Instance);
        return m?.DeclaringType == t ? m : null;
    }

    [Fact]
    public void TheAssembly_ActuallyHasFinalizableWrappers_SoTheTheoryIsNotVacuous()
    {
        var names = FinalizableTypeNames();
        Assert.Contains(typeof(JuceAudioPlugin).FullName, names);
        Assert.Contains(typeof(JuceAudioSource).FullName, names);
        Assert.Contains(typeof(JuceAudioEngine).FullName, names);
        Assert.Contains(typeof(JuceMidiInput).FullName, names);
        Assert.Contains(typeof(JuceMidiOutput).FullName, names);
    }

    [Theory]
    [MemberData(nameof(FinalizableTypes))]
    public void Finalizer_OnAPartiallyConstructedInstance_DoesNotThrow(string typeName)
    {
        var type = typeof(JuceAudioEngine).Assembly.GetType(typeName, throwOnError: true)!;
        // Exactly the state a constructor that threw leaves behind: allocated,
        // finalizable, every field at its default.
        var zombie = RuntimeHelpers.GetUninitializedObject(type);
        try
        {
            var ex = Record.Exception(() => FinalizerOf(type)!.Invoke(zombie, null));
            // Reflection wraps whatever the finalizer threw.
            Assert.Null(ex is TargetInvocationException tie ? tie.InnerException : ex);
        }
        finally
        {
            // We just ran it by hand — do not let the runtime run it again.
            GC.SuppressFinalize(zombie);
        }
    }

    [Fact]
    public void RejectedPluginHandle_LeavesNothingOnTheFinalizerQueue()
    {
        // The end-to-end reproduction. Before the fix this did not report a
        // failed assertion — it took the process with it, here or in whichever
        // unrelated test happened to be running when the GC got round to it.
        Assert.Throws<ArgumentException>(() => new JuceAudioPlugin(nint.Zero, "x.vst3"));

        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        GC.WaitForPendingFinalizers();
    }
}
