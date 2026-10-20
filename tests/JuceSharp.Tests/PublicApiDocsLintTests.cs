using System.ComponentModel;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml.Linq;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// The public API of the six shipping packages is documented, and STAYS documented.
/// <para>
/// The compiler is the primary enforcement: <c>CS1591</c> is no longer suppressed in
/// <c>Directory.Build.props</c>, so an undocumented public member is a warning at the
/// site that introduced it. This class is the backstop for the two things a warning
/// cannot catch — someone putting the <c>NoWarn</c> back, and a "documented" member whose
/// summary says nothing ("Gets or sets the Volume.") — plus an independent reflection
/// sweep that reads the SHIPPED <c>.xml</c> rather than the source, because the
/// <c>.xml</c> is what a consumer's IDE actually shows.
/// </para>
/// </summary>
public sealed class PublicApiDocsLintTests
{
    private static readonly string[] ShippingAssemblies =
    {
        "JuceSharp",
        "JuceSharp.Dsp",
        "JuceSharp.AudioIo",
        "JuceSharp.StemSeparation",
        "JuceSharp.StemSeparation.Onnx",
        "juce-stems-diag",   // JuceSharp.Diagnostics packs as a dotnet tool under this name
    };

    // ---------------------------------------------------------------- helpers

    private static string RepoRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "JuceSharp.sln")))
            dir = dir.Parent;
        Assert.NotNull(dir);
        return dir!.FullName;
    }

    private static Assembly Load(string simpleName)
    {
        var path = Path.Combine(AppContext.BaseDirectory, simpleName + ".dll");
        Assert.True(File.Exists(path), $"{simpleName}.dll is not beside the test assembly.");
        return Assembly.LoadFrom(path);
    }

    /// <summary>Every <c>name</c> attribute in one shipped doc file.</summary>
    private static (HashSet<string> Ids, XDocument Doc) DocFile(string simpleName)
    {
        var path = Path.Combine(AppContext.BaseDirectory, simpleName + ".xml");
        Assert.True(File.Exists(path),
            $"{simpleName}.xml was not produced/copied. GenerateDocumentationFile must stay on " +
            "for every packable project, and the reference must flow the doc file to the output.");
        var doc = XDocument.Load(path);
        var ids = doc.Descendants("member")
                     .Select(m => (string?)m.Attribute("name"))
                     .Where(n => !string.IsNullOrEmpty(n))
                     .Select(n => n!)
                     .ToHashSet(StringComparer.Ordinal);
        return (ids, doc);
    }

    // ---------------------------------------------------- doc-comment id math
    // ECMA-334 Annex E. Only the shapes this API actually uses are implemented;
    // anything unrecognised throws rather than silently producing an id that can
    // never match, which would make the sweep pass vacuously.

    private static string TypeId(Type t) => "T:" + TypeName(t);

    private static string TypeName(Type t)
    {
        var name = t.FullName ?? t.Name;
        // Nested types are 'Outer+Inner' in reflection and 'Outer.Inner' in doc ids.
        return name.Replace('+', '.');
    }

    private static string ParamType(Type t, Type[] typeArgs, Type[] methodArgs)
    {
        if (t.IsByRef) return ParamType(t.GetElementType()!, typeArgs, methodArgs) + "@";
        if (t.IsPointer) return ParamType(t.GetElementType()!, typeArgs, methodArgs) + "*";
        if (t.IsArray)
        {
            var rank = t.GetArrayRank();
            var suffix = rank == 1 ? "[]" : "[" + string.Join(",", Enumerable.Repeat("0:", rank)) + "]";
            return ParamType(t.GetElementType()!, typeArgs, methodArgs) + suffix;
        }
        if (t.IsGenericParameter)
        {
            var i = Array.FindIndex(methodArgs, a => a.Name == t.Name && a.DeclaringMethod is not null);
            if (t.DeclaringMethod is not null && i >= 0) return "``" + i;
            var j = Array.FindIndex(typeArgs, a => a.Name == t.Name);
            return "`" + (j >= 0 ? j : t.GenericParameterPosition);
        }
        if (t.IsConstructedGenericType)
        {
            var open = t.GetGenericTypeDefinition().FullName!.Replace('+', '.');
            var tick = open.IndexOf('`');
            if (tick >= 0) open = open[..tick];
            var args = t.GetGenericArguments().Select(a => ParamType(a, typeArgs, methodArgs));
            return open + "{" + string.Join(",", args) + "}";
        }
        return TypeName(t);
    }

    private static string Signature(ParameterInfo[] ps, Type declaring, MethodBase? m)
    {
        if (ps.Length == 0) return string.Empty;
        var typeArgs = declaring.IsGenericType ? declaring.GetGenericArguments() : Type.EmptyTypes;
        var methodArgs = m is { IsGenericMethodDefinition: true } ? m.GetGenericArguments() : Type.EmptyTypes;
        return "(" + string.Join(",", ps.Select(p => ParamType(p.ParameterType, typeArgs, methodArgs))) + ")";
    }

    private static string MemberId(MemberInfo member)
    {
        var declaring = member.DeclaringType!;
        var owner = TypeName(declaring);
        switch (member)
        {
            case FieldInfo f:
                return $"F:{owner}.{f.Name}";
            case EventInfo e:
                return $"E:{owner}.{e.Name}";
            case PropertyInfo p:
                return $"P:{owner}.{p.Name}{Signature(p.GetIndexParameters(), declaring, null)}";
            case ConstructorInfo c:
                return $"M:{owner}.#ctor{Signature(c.GetParameters(), declaring, c)}";
            case MethodInfo mi:
            {
                var name = mi.Name.Replace('.', '#');   // explicit interface impls
                var arity = mi.IsGenericMethodDefinition ? "``" + mi.GetGenericArguments().Length : string.Empty;
                var id = $"M:{owner}.{name}{arity}{Signature(mi.GetParameters(), declaring, mi)}";
                // Conversion operators are disambiguated by their return type.
                if (mi.Name is "op_Implicit" or "op_Explicit")
                    id += "~" + ParamType(mi.ReturnType, declaring.IsGenericType ? declaring.GetGenericArguments() : Type.EmptyTypes, Type.EmptyTypes);
                return id;
            }
            default:
                throw new InvalidOperationException($"Unhandled member kind {member.MemberType} for {member.Name}.");
        }
    }

    // -------------------------------------------------------------- filtering

    private static bool IsCompilerSupplied(MemberInfo m)
    {
        if (m.GetCustomAttribute<CompilerGeneratedAttribute>() is not null) return true;
        if (m.GetCustomAttribute<EditorBrowsableAttribute>() is { State: EditorBrowsableState.Never }) return true;

        // Record and tuple plumbing Roslyn synthesizes. None of it is in the .xml,
        // and none of it produces CS1591 either — the two agree, which is the point.
        if (m.Name.StartsWith('<')) return true;                        // <Clone>$
        if (m is MethodBase { IsSpecialName: true } and not ConstructorInfo) return true;
        if (m.DeclaringType is not null && IsRecord(m.DeclaringType) && m.Name is
            "Equals" or "GetHashCode" or "ToString" or "PrintMembers" or "Deconstruct" or "op_Equality" or "op_Inequality")
            return true;
        if (m.Name == "EqualityContract") return true;
        if (m is MethodInfo { Name: "Finalize" }) return true;          // ~Foo(); documented on the source, not emitted as its own id

        return false;
    }

    private static bool IsRecord(Type t) =>
        t.GetMethod("<Clone>$", BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance) is not null
        || t.GetMethod("PrintMembers", BindingFlags.NonPublic | BindingFlags.Instance) is not null;

    /// <summary>
    /// Names of a positional record's primary-constructor properties, read off the
    /// synthesized <c>Deconstruct</c> (which exists only for positional records and whose
    /// out-parameter names ARE the property names). Roslyn documents these through
    /// <c>&lt;param&gt;</c> on the TYPE and emits no <c>P:</c> entry for them — and raises
    /// no CS1591 either, so excluding them keeps this sweep in step with the compiler
    /// rather than ahead of it.
    /// </summary>
    private static HashSet<string> PositionalProperties(Type t)
    {
        var deconstruct = t.GetMethod("Deconstruct", BindingFlags.Public | BindingFlags.Instance);
        return deconstruct is null
            ? new HashSet<string>(StringComparer.Ordinal)
            : deconstruct.GetParameters().Select(p => p.Name ?? string.Empty).ToHashSet(StringComparer.Ordinal);
    }

    /// <summary>
    /// A parameterless constructor with no source of its own — the implicit default ctor,
    /// or a positional record's own synthesized one. There is nothing to attach a comment
    /// to, so neither the <c>.xml</c> nor CS1591 knows about it.
    /// </summary>
    private static bool IsImplicitDefaultConstructor(MemberInfo m) =>
        m is ConstructorInfo { IsStatic: false } c && c.GetParameters().Length == 0;

    private static bool IsPubliclyVisible(Type t)
    {
        while (t.IsNested)
        {
            if (!t.IsNestedPublic) return false;
            t = t.DeclaringType!;
        }
        return t.IsPublic;
    }

    private static IEnumerable<MemberInfo> PublicApi(Assembly asm)
    {
        foreach (var t in asm.GetTypes().Where(IsPubliclyVisible).OrderBy(t => t.FullName, StringComparer.Ordinal))
        {
            if (t.GetCustomAttribute<CompilerGeneratedAttribute>() is not null) continue;
            if (t.GetCustomAttribute<EditorBrowsableAttribute>() is { State: EditorBrowsableState.Never }) continue;
            yield return t;

            const BindingFlags flags = BindingFlags.Public | BindingFlags.Instance |
                                       BindingFlags.Static | BindingFlags.DeclaredOnly;
            var positional = PositionalProperties(t);
            foreach (var m in t.GetMembers(flags))
            {
                if (m is Type) continue;                       // nested types come round on their own pass
                if (IsCompilerSupplied(m)) continue;
                if (IsImplicitDefaultConstructor(m)) continue;
                if (m is PropertyInfo && positional.Contains(m.Name)) continue;
                // Delegate plumbing: Invoke/BeginInvoke/EndInvoke/.ctor on a delegate type.
                if (typeof(Delegate).IsAssignableFrom(t)) continue;
                // Enum members are fields; the synthesized value__ backing field is not API.
                if (m is FieldInfo { Name: "value__" }) continue;
                yield return m;
            }
        }
    }

    // ------------------------------------------------------------------ tests

    /// <summary>
    /// (a) Every public type and member of the six shipping assemblies has a
    /// <c>&lt;member&gt;</c> entry in that assembly's shipped <c>.xml</c>.
    /// <para>This reads the BUILT doc file, not the source, so it also catches a project
    /// that quietly stopped generating one.</para>
    /// </summary>
    [Fact]
    public void Every_public_member_of_every_shipping_assembly_is_in_its_shipped_xml()
    {
        var missing = new List<string>();
        var checkedCount = 0;

        foreach (var simpleName in ShippingAssemblies)
        {
            var asm = Load(simpleName);
            var (ids, _) = DocFile(simpleName);

            foreach (var member in PublicApi(asm))
            {
                var id = member is Type t ? TypeId(t) : MemberId(member);
                checkedCount++;
                if (!ids.Contains(id)) missing.Add($"{simpleName}: {id}");
            }
        }

        // Non-vacuity floor: this API is ~700 documented entries. A generator bug that
        // produced nothing would otherwise make the assertion below pass trivially.
        Assert.True(checkedCount > 400, $"Only {checkedCount} public members were examined; the sweep found nothing to check.");
        Assert.True(missing.Count == 0,
            $"{missing.Count} public member(s) carry no XML documentation:{Environment.NewLine}" +
            string.Join(Environment.NewLine, missing.Take(40)));
    }

    /// <summary>
    /// (b) No summary is a placeholder. "Gets or sets the Volume." restates the signature
    /// and tells a reader nothing they could not see; an empty summary is worse. Units, the
    /// threading contract, what it throws, and the <c>dca_*</c> export it maps to are what
    /// the doc is for.
    /// </summary>
    [Fact]
    public void No_summary_is_a_placeholder_restatement_of_the_member_name()
    {
        // "Gets the Volume." / "Sets the Volume" / "Gets or sets the Volume." / "The Volume."
        var placeholder = new Regex(
            @"^((Gets|Sets|Gets or sets)\s+(the\s+)?\w+|The\s+\w+|Gets or sets a value indicating whether)\.?$",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

        var offenders = new List<string>();
        var summaries = 0;

        foreach (var simpleName in ShippingAssemblies)
        {
            var (_, doc) = DocFile(simpleName);
            foreach (var member in doc.Descendants("member"))
            {
                var name = (string?)member.Attribute("name") ?? "?";
                var summary = member.Element("summary");
                if (summary is null) continue;      // inheritdoc-only members carry no summary
                summaries++;

                var text = Regex.Replace(summary.Value, @"\s+", " ").Trim();
                if (text.Length == 0)
                {
                    offenders.Add($"{simpleName}: {name} — empty <summary>");
                    continue;
                }
                if (placeholder.IsMatch(text))
                    offenders.Add($"{simpleName}: {name} — placeholder summary \"{text}\"");
            }
        }

        Assert.True(summaries > 400, $"Only {summaries} summaries were examined; the sweep found nothing to check.");
        Assert.True(offenders.Count == 0,
            $"{offenders.Count} placeholder summary/summaries:{Environment.NewLine}" +
            string.Join(Environment.NewLine, offenders.Take(40)));
    }

    /// <summary>
    /// (c) Nothing suppresses CS1591 again. The warning is the primary enforcement — a
    /// <c>&lt;NoWarn&gt;</c> anywhere in the props or in a packable csproj turns the whole
    /// sweep back into a one-off cleanup.
    /// </summary>
    [Fact]
    public void Nothing_suppresses_CS1591_in_the_props_or_in_any_packable_project()
    {
        var root = RepoRoot();
        var files = new List<string> { Path.Combine(root, "Directory.Build.props") };

        foreach (var csproj in Directory.EnumerateFiles(Path.Combine(root, "src"), "*.csproj", SearchOption.AllDirectories))
        {
            var rel = csproj.Replace('\\', '/');
            if (rel.Contains("/build/") || rel.Contains("/_deps/") || rel.Contains("/third_party/")
                || rel.Contains("/bin/") || rel.Contains("/obj/")) continue;
            var doc = XDocument.Load(csproj);
            var packable = doc.Descendants("IsPackable").Select(e => e.Value).FirstOrDefault();
            if (string.Equals(packable, "false", StringComparison.OrdinalIgnoreCase)) continue;
            files.Add(csproj);
        }

        // W5-P4: the sweep has to reach the files with the MOST leverage, and it
        // did not. `PackagingLintTests` bans a <NoWarn> element only in csprojs
        // under src/tests/examples; nothing read Directory.Build.props' siblings
        // or the two NESTED props files — which is exactly where one line would
        // switch CS1591 off for everything.
        foreach (var extra in new[]
                 {
                     Path.Combine(root, "Directory.Build.targets"),
                     Path.Combine(root, "tests", "Directory.Build.props"),
                     Path.Combine(root, "examples", "Directory.Build.props"),
                     Path.Combine(root, ".editorconfig"),
                     Path.Combine(root, ".globalconfig"),
                 })
            if (File.Exists(extra)) files.Add(extra);

        // The props plus the six packable projects plus the extras above.
        Assert.True(files.Count >= 7, $"Only found {files.Count} file(s) to lint; the project sweep is not reaching src/.");

        // MSBuild accepts the BARE NUMBER — `<NoWarn>1591</NoWarn>`, which is
        // what Visual Studio's "Suppress warnings" box writes — and it
        // suppresses CS1591 identically. A string match on "CS1591" walks
        // straight past it, which is how this guard was vacuous against the one
        // form a human is most likely to produce. Also catch the analyzer-config
        // route, which no test touched at all.
        var suppressed = new Regex(@"\b(CS)?1591\b", RegexOptions.IgnoreCase);
        var severityOff = new Regex(@"dotnet_diagnostic\.CS1591\.severity\s*=\s*(none|silent|suggestion)",
                                    RegexOptions.IgnoreCase);

        foreach (var f in files)
        {
            var text = File.ReadAllText(f, Encoding.UTF8);
            // Strip XML comments: the props file EXPLAINS why CS1591 is enforced, and that
            // sentence is not a suppression. (# comments for the editorconfig half.)
            var code = Regex.Replace(text, "<!--.*?-->", string.Empty, RegexOptions.Singleline);
            code = Regex.Replace(code, @"^\s*#.*$", string.Empty, RegexOptions.Multiline);

            foreach (Match noWarn in Regex.Matches(code, @"<(NoWarn|WarningsNotAsErrors)>(?<v>[^<]*)</\1>",
                                                   RegexOptions.IgnoreCase))
                Assert.False(suppressed.IsMatch(noWarn.Groups["v"].Value),
                    $"{Path.GetFileName(f)} suppresses CS1591 via <{noWarn.Groups[1].Value}>"
                    + $"{noWarn.Groups["v"].Value}</{noWarn.Groups[1].Value}>. The bare-number form "
                    + "counts — MSBuild treats 1591 and CS1591 identically.");

            Assert.False(severityOff.IsMatch(code),
                $"{Path.GetFileName(f)} turns CS1591 off through an analyzer config severity.");

            Assert.DoesNotContain("CS1591", code, StringComparison.Ordinal);
        }
    }
}
