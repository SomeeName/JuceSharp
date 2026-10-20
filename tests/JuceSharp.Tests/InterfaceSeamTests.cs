using System.Diagnostics;
using System.Reflection;
using JuceSharp.Dsp;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
///     The 0.9 test seams (audit F9): every wrapper type is <c>sealed</c> with an
///     <c>internal</c> constructor, so a consumer could not unit-test the code
///     that DRIVES the engine without <c>juce_sharp.dll</c> and a sound card.
///     Each concrete type now implements an interface covering its public
///     instance surface.
///     <para>
///         <b>The lint is the point.</b> An interface extracted once and then
///         forgotten is worse than none: a member added to
///         <see cref="JuceAudioEngine" /> and not to
///         <see cref="IJuceAudioEngine" /> is invisible to every fake, so the
///         seam silently stops covering the thing that was just added. This
///         walks the members by reflection in BOTH directions.
///     </para>
/// </summary>
[Collection("NativeEngine")]
public sealed class InterfaceSeamTests
{
    public static TheoryData<Type, Type> Pairs => new()
    {
        { typeof(JuceAudioEngine), typeof(IJuceAudioEngine) },
        { typeof(JuceAudioSource), typeof(IJuceAudioSource) },
        { typeof(JuceAudioPlugin), typeof(IJuceAudioPlugin) },
        { typeof(JuceMidiInput), typeof(IJuceMidiInput) },
        { typeof(JuceMidiOutput), typeof(IJuceMidiOutput) },
        { typeof(JuceDspChain), typeof(IJuceDspChain) },
        { typeof(JuceDspNode), typeof(IJuceDspNode) },
    };

    [Theory]
    [MemberData(nameof(Pairs))]
    public void EveryPublicInstanceMember_IsOnTheInterface(Type concrete, Type contract)
    {
        Assert.True(contract.IsAssignableFrom(concrete),
            $"{concrete.Name} does not implement {contract.Name}.");

        var ifaceMembers = InterfaceMembers(contract).ToArray();
        Assert.NotEmpty(ifaceMembers);

        var missing = new List<string>();

        foreach (var p in concrete.GetProperties(Declared))
        {
            var match = ifaceMembers.OfType<PropertyInfo>().FirstOrDefault(
                i => i.Name == p.Name
                     && Compatible(p.PropertyType, i.PropertyType)
                     && SameIndexers(p, i));
            if (match is null) { missing.Add($"property {Describe(p)}"); continue; }
            if (p.CanRead && !match.CanRead) missing.Add($"property {p.Name} has no getter on {contract.Name}");
            if (p.CanWrite && match.SetMethod is null && p.SetMethod!.IsPublic)
                missing.Add($"property {p.Name} has no setter on {contract.Name}");
        }

        foreach (var e in concrete.GetEvents(Declared))
        {
            if (!ifaceMembers.OfType<EventInfo>().Any(i => i.Name == e.Name && i.EventHandlerType == e.EventHandlerType))
                missing.Add($"event {e.Name}");
        }

        foreach (var m in concrete.GetMethods(Declared))
        {
            if (m.IsSpecialName) continue;           // property/event accessors
            if (IsObjectOverride(m)) continue;       // ToString / Equals / GetHashCode
            if (!ifaceMembers.OfType<MethodInfo>().Any(i => Matches(m, i)))
                missing.Add($"method {Describe(m)}");
        }

        Assert.True(missing.Count == 0,
            $"{contract.Name} is missing {missing.Count} public member(s) of {concrete.Name}:"
            + Environment.NewLine + string.Join(Environment.NewLine, missing));
    }

    /// <summary>
    /// The other direction: an interface may not declare something the type does
    /// not have under the same NAME. The compiler already proves it is
    /// implemented, but an EXPLICIT implementation satisfies it silently — which
    /// would let the interface drift into a second, differently-named surface.
    /// </summary>
    [Theory]
    [MemberData(nameof(Pairs))]
    public void TheInterfaceDeclaresNothingTheTypeDoesNotHave(Type concrete, Type contract)
    {
        var publicNames = concrete
            .GetMembers(Declared)
            .Select(m => m.Name)
            .ToHashSet(StringComparer.Ordinal);

        var strays = InterfaceMembers(contract)
            .Select(m => m.Name)
            .Where(n => !n.StartsWith("get_", StringComparison.Ordinal)
                        && !n.StartsWith("set_", StringComparison.Ordinal)
                        && !n.StartsWith("add_", StringComparison.Ordinal)
                        && !n.StartsWith("remove_", StringComparison.Ordinal))
            .Where(n => !publicNames.Contains(n))
            .Distinct(StringComparer.Ordinal)
            .ToArray();

        Assert.True(strays.Length == 0,
            $"{contract.Name} declares members {concrete.Name} does not expose publicly: {string.Join(", ", strays)}");
    }

    // ------------------------------------------------------------- the example

    /// <summary>
    ///     THE WHOLE POINT, end to end: a consumer written against
    ///     <see cref="IJuceAudioEngine" /> is driven by a hand-written fake, and
    ///     <b>no native library is loaded to do it</b> — the module list of this
    ///     process gains nothing, and <c>DCA_ENABLE_NULL_DEVICE</c> is never set,
    ///     so there is no device either.
    ///     <para>
    ///         The module comparison is a before/after DELTA on purpose: another
    ///         test in the same host may already have loaded
    ///         <c>juce_sharp.dll</c>, and what is being asserted is that THIS test
    ///         did not need it.
    ///     </para>
    /// </summary>
    [Fact]
    public void AConsumerRunsOnAFakeEngine_WithoutLoadingTheNativeLibrary()
    {
        var before = NativeModules();

        var engine = new FakeJuceAudioEngine();
        var player = new PracticeLoopPlayer(engine);

        player.Start(@"C:\songs\nothing-here.ogg", from: TimeSpan.FromSeconds(30));

        Assert.Equal(@"C:\songs\nothing-here.ogg", engine.OpenedPath);
        Assert.Equal(TimeSpan.FromSeconds(30), engine.NextSource.Position);
        Assert.True(engine.NextSource.Playing);
        Assert.Contains("Start", engine.Calls);

        player.Dispose();
        Assert.True(engine.NextSource.Disposed);
        Assert.False(engine.Disposed, "the consumer does not own the engine and must not dispose it");

        // Nothing native arrived because of any of that. A strict module COUNT
        // would be flaky (the runtime loads managed assemblies lazily, and they
        // are modules too), so the assertion is about the library this package
        // would have needed: juce_sharp.dll, and the ORT / demucs natives beside
        // it.
        var arrived = NativeModules().Except(before).ToArray();
        Assert.DoesNotContain(arrived, m => m.Contains("juce", StringComparison.OrdinalIgnoreCase));
        Assert.DoesNotContain(arrived, m => m.Contains("onnxruntime", StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>
    /// A miniature of the code a host actually has to test: open, seek, play,
    /// tear down. It names no concrete wrapper type anywhere.
    /// </summary>
    private sealed class PracticeLoopPlayer : IDisposable
    {
        private readonly IJuceAudioEngine _engine;
        private IJuceAudioSource? _source;

        public PracticeLoopPlayer(IJuceAudioEngine engine) => _engine = engine;

        public void Start(string path, TimeSpan from)
        {
            _engine.Start();
            _source = _engine.OpenFile(path);
            _source.Seek(from);
            _source.Play();
        }

        public void Dispose()
        {
            _source?.Dispose();
            _source = null;
        }
    }

    // ---------------------------------------------------------------- helpers

    private const BindingFlags Declared =
        BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly;

    private static IEnumerable<MemberInfo> InterfaceMembers(Type contract)
        => contract.GetMembers().Concat(contract.GetInterfaces().SelectMany(i => i.GetMembers()));

    private static bool IsObjectOverride(MethodInfo m)
        => m.GetBaseDefinition().DeclaringType == typeof(object);

    private static bool SameIndexers(PropertyInfo a, PropertyInfo b)
        => a.GetIndexParameters().Length == b.GetIndexParameters().Length;

    private static bool Matches(MethodInfo concrete, MethodInfo contract)
    {
        if (concrete.Name != contract.Name) return false;
        var cp = concrete.GetParameters();
        var ip = contract.GetParameters();
        if (cp.Length != ip.Length) return false;
        for (var i = 0; i < cp.Length; i++)
        {
            if (cp[i].IsOut != ip[i].IsOut) return false;
            if (cp[i].ParameterType.IsByRef != ip[i].ParameterType.IsByRef) return false;
            // Parameters are CONTRAVARIANT: the interface may accept a wider type
            // (IJuceDspNode where the class takes JuceDspNode).
            if (!Compatible(cp[i].ParameterType, ip[i].ParameterType)) return false;
        }
        return Compatible(concrete.ReturnType, contract.ReturnType);
    }

    /// <summary>
    /// Is <paramref name="concrete" /> an acceptable stand-in for
    /// <paramref name="contract" />? Identical, assignable (the
    /// concrete-returns-JuceDspNode / interface-returns-IJuceDspNode case, which
    /// C# expresses as an explicit implementation), or the same relationship one
    /// <c>Task&lt;T&gt;</c> deep, which is how an async factory reads.
    /// </summary>
    private static bool Compatible(Type concrete, Type contract)
    {
        if (concrete.IsByRef) concrete = concrete.GetElementType()!;
        if (contract.IsByRef) contract = contract.GetElementType()!;
        if (concrete == contract) return true;
        if (contract.IsAssignableFrom(concrete)) return true;
        if (concrete.IsGenericType && contract.IsGenericType
            && concrete.GetGenericTypeDefinition() == typeof(Task<>)
            && contract.GetGenericTypeDefinition() == typeof(Task<>))
        {
            return Compatible(concrete.GetGenericArguments()[0], contract.GetGenericArguments()[0]);
        }
        return false;
    }

    private static string Describe(MethodInfo m)
        => $"{m.Name}({string.Join(", ", m.GetParameters().Select(p => p.ParameterType.Name))})";

    private static string Describe(PropertyInfo p) => p.Name;

    private static HashSet<string> NativeModules()
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (ProcessModule module in Process.GetCurrentProcess().Modules)
            names.Add(module.ModuleName);
        return names;
    }
}
