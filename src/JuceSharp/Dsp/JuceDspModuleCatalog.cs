using System.Globalization;
using System.Text;
using JuceSharp.Interop;

namespace JuceSharp.Dsp;

/// <summary>
///     One float parameter of a DSP module type, straight out of the native
///     catalogue. A generic pedalboard UI needs no per-module code: this record
///     says how to render the control.
///     <para>
///         Every DISCRETE param declares itself as exactly one of two shapes.
///         NAMED — the value picks a named thing (amp voicing, tremolo shape, a
///         footswitch) — carries <see cref="Labels" /> and wants a PICKER. ORDINAL
///         — the value is a count (chorus voices, phaser stages, bitcrusher bits)
///         — carries a <see cref="Step" /> and wants a knob that SNAPS, because a
///         continuous knob shows "6.5 stages" while the core runs 6. Note phaser
///         "stages" has step 2, not 1: the core forces the allpass count even.
///     </para>
/// </summary>
/// <param name="Id">Persisted parameter id, e.g. <c>"mix"</c>.</param>
/// <param name="Name">Display name.</param>
/// <param name="Min">Inclusive minimum.</param>
/// <param name="Max">Inclusive maximum.</param>
/// <param name="Default">The value a freshly added node starts at.</param>
/// <param name="Step">Quantization step for an ORDINAL discrete param; 0 when continuous.</param>
/// <param name="Labels">Value labels for a NAMED discrete param (value = <paramref name="Min"/> + index); null when it has none.</param>
public sealed record JuceDspParamInfo(
    string Id,
    string Name,
    float Min,
    float Max,
    float Default,
    float Step,
    IReadOnlyList<string>? Labels)
{
    /// <summary>True when the DSP only ever runs whole values of this param —
    /// either because it is NAMED (<see cref="Labels"/>) or ORDINAL
    /// (<see cref="Step"/> &gt; 0).</summary>
    public bool IsDiscrete => IsNamed || IsOrdinal;

    /// <summary>Render a picker: the value selects a named thing.</summary>
    public bool IsNamed => Labels is { Count: > 0 };

    /// <summary>Render a snapping knob: the value is a count.</summary>
    public bool IsOrdinal => Step > 0f;

    /// <summary>How this value should read to a human — the label for a NAMED
    /// param, an integer for an ORDINAL one, two decimals otherwise.</summary>
    public string Describe(float value)
    {
        if (IsNamed)
        {
            var index = (int)MathF.Round(value - Min);
            if (index >= 0 && index < Labels!.Count) return Labels[index];
        }
        return value.ToString(IsOrdinal ? "F0" : "F2", CultureInfo.InvariantCulture);
    }
}

/// <summary>
/// A string-valued parameter of a module type (a NAM capture path, a user IR
/// WAV, a Miku phrase table). <paramref name="FileFilter"/> is a
/// <c>';'</c>-separated wildcard list for a file picker (e.g.
/// <c>"*.nam;*.aidax"</c>), empty when the value is not a path.
/// </summary>
public sealed record JuceDspStringParamInfo(string Id, string Name, string FileFilter);

/// <summary>One entry of the native DSP module catalogue.</summary>
/// <param name="Id">Persisted type id, e.g. <c>"delay"</c>. This is what
/// <see cref="JuceDspChain.AddNode(string)"/> takes.</param>
/// <param name="DisplayName">Human label, e.g. <c>"Delay"</c>.</param>
/// <param name="Category">
///     Which drawer of a palette this module belongs in, out of a short FIXED
///     set the ENGINE owns: <c>dynamics</c>, <c>drive</c>, <c>amp</c>,
///     <c>cab</c>, <c>modulation</c>, <c>delay</c>, <c>reverb</c>,
///     <c>pitch</c>, <c>filter</c>, <c>eq</c>, <c>utility</c>, <c>routing</c>,
///     <c>synth</c>. Read from <c>dca_dsp_module_type_category</c> (ABI v33) and
///     never from a managed guess table, which would be a second source of truth
///     for something only the native factory knows. Every catalogue entry has
///     one; empty means the native DLL predates v33.
/// </param>
/// <param name="Params">Float params, in catalogue order.</param>
/// <param name="StringParams">String params, in catalogue order.</param>
/// <param name="SupportsSidechain">
///     Whether <see cref="JuceDspNode.Sidechain" /> accepts a source other than
///     <see cref="JuceDspSidechainSource.None" /> on this module type (ABI v34,
///     <c>dca_dsp_module_type_flags</c> bit 0). The engine REJECTS a key on any
///     other type, so a palette must not offer the control for one — a source a
///     module silently ignored would read as a live feature that does nothing.
///     False on a native DLL that predates v34.
/// </param>
public sealed record JuceDspModuleInfo(
    string Id,
    string DisplayName,
    string Category,
    IReadOnlyList<JuceDspParamInfo> Params,
    IReadOnlyList<JuceDspStringParamInfo> StringParams,
    bool SupportsSidechain = false);

/// <summary>
///     The static DSP module catalogue. NO ENGINE REQUIRED — this is what a
///     pedalboard UI enumerates to build its palette before any device is open,
///     and it is why a fully catalogue-driven rig UI needs no per-module code.
///     <para>
///         Read once from native and cached for the process: the catalogue is a
///         compile-time table on the other side, so re-reading it would be dozens
///         of P/Invokes per UI frame for an answer that cannot change.
///     </para>
/// </summary>
public static class JuceDspModuleCatalog
{
    private static readonly Lazy<IReadOnlyList<JuceDspModuleInfo>> s_modules =
        new(Load, LazyThreadSafetyMode.ExecutionAndPublication);

    private static readonly Lazy<IReadOnlyDictionary<string, JuceDspModuleInfo>> s_byId =
        new(() => s_modules.Value.ToDictionary(m => m.Id, StringComparer.Ordinal),
            LazyThreadSafetyMode.ExecutionAndPublication);

    /// <summary>Every module type the native engine can build, in catalogue
    /// order.</summary>
    /// <exception cref="DllNotFoundException">The native DLL is not deployed.</exception>
    public static IReadOnlyList<JuceDspModuleInfo> Modules => s_modules.Value;

    /// <summary>Look one module type up by its persisted id, or null when the
    /// engine has no such type (a preset from a newer build, say).</summary>
    public static JuceDspModuleInfo? Find(string moduleTypeId)
        => moduleTypeId is not null && s_byId.Value.TryGetValue(moduleTypeId, out var m) ? m : null;

    /// <summary>ABI v34 capability bits. A DLL without the export is not an
    /// error — it is an older engine, and the honest answer there is "no".</summary>
    private static bool LoadSupportsSidechain(int index)
    {
        try
        {
            return NativeMethods.DspModuleTypeFlags(index, out var flags) == NativeMethods.DcaStatus.Ok
                   && (flags & NativeMethods.DspModuleFlagSidechain) != 0;
        }
        catch (EntryPointNotFoundException) { return false; }
    }

    private static IReadOnlyList<JuceDspModuleInfo> Load()
    {
        var count = NativeMethods.DspModuleTypeCount();
        if (count <= 0) return Array.Empty<JuceDspModuleInfo>();

        var list = new List<JuceDspModuleInfo>(count);
        var idBuf = new byte[64];
        var nameBuf = new byte[128];
        var categoryBuf = new byte[64];
        for (var i = 0; i < count; i++)
        {
            if (NativeMethods.DspModuleTypeInfo(i, idBuf, idBuf.Length, nameBuf, nameBuf.Length)
                != NativeMethods.DcaStatus.Ok) continue;
            var id = Utf8(idBuf);
            if (id.Length == 0) continue;
            list.Add(new JuceDspModuleInfo(id, Utf8(nameBuf), LoadCategory(i, categoryBuf),
                LoadParams(id), LoadStringParams(id), LoadSupportsSidechain(i)));
        }
        return list;
    }

    /// <summary>ABI v33. One fixed-vocabulary word, so a 64-byte buffer is
    /// generous and the size-then-copy round trip the other getters need is
    /// wasted work — but the length is still honoured, and a value that would
    /// not fit is reported as unknown rather than as a truncated word a UI would
    /// group under its own drawer.</summary>
    private static string LoadCategory(int index, byte[] buf)
    {
        if (NativeMethods.DspModuleTypeCategory(index, buf, buf.Length, out var len)
            != NativeMethods.DcaStatus.Ok) return string.Empty;
        return len > 0 && len < buf.Length ? Utf8(buf) : string.Empty;
    }

    private static IReadOnlyList<JuceDspParamInfo> LoadParams(string moduleTypeId)
    {
        var count = NativeMethods.DspModuleParamCount(moduleTypeId);
        if (count <= 0) return Array.Empty<JuceDspParamInfo>();

        var list = new List<JuceDspParamInfo>(count);
        var idBuf = new byte[64];
        var nameBuf = new byte[128];
        for (var i = 0; i < count; i++)
        {
            if (NativeMethods.DspModuleParamInfo(moduleTypeId, i, idBuf, idBuf.Length, nameBuf, nameBuf.Length,
                    out var min, out var max, out var def) != NativeMethods.DcaStatus.Ok) continue;
            var paramId = Utf8(idBuf);
            if (paramId.Length == 0) continue;

            // -1 is the "no labels, plain numeric param" sentinel for both of
            // these; folding it to 0 would claim a labelled param with no labels.
            var step = NativeMethods.DspModuleParamStep(moduleTypeId, paramId);
            list.Add(new JuceDspParamInfo(paramId, Utf8(nameBuf), min, max, def,
                step > 0f ? step : 0f, LoadLabels(moduleTypeId, paramId)));
        }
        return list;
    }

    private static IReadOnlyList<string>? LoadLabels(string moduleTypeId, string paramId)
    {
        var labelCount = NativeMethods.DspModuleParamLabelCount(moduleTypeId, paramId);
        if (labelCount <= 0) return null;

        var labels = new string[labelCount];
        for (var v = 0; v < labelCount; v++)
        {
            // Size-then-copy, the contract every out-buffer on this ABI uses:
            // the first call returns the FULL byte length excluding the NUL.
            var len = NativeMethods.DspModuleParamLabel(moduleTypeId, paramId, v, null, 0);
            if (len < 0) { labels[v] = string.Empty; continue; }
            var buf = new byte[len + 1];
            NativeMethods.DspModuleParamLabel(moduleTypeId, paramId, v, buf, buf.Length);
            labels[v] = Utf8(buf);
        }
        return labels;
    }

    private static IReadOnlyList<JuceDspStringParamInfo> LoadStringParams(string moduleTypeId)
    {
        var count = NativeMethods.DspModuleStringParamCount(moduleTypeId);
        if (count <= 0) return Array.Empty<JuceDspStringParamInfo>();

        var list = new List<JuceDspStringParamInfo>(count);
        var idBuf = new byte[64];
        var nameBuf = new byte[128];
        var filterBuf = new byte[128];
        for (var i = 0; i < count; i++)
        {
            if (NativeMethods.DspModuleStringParamInfo(moduleTypeId, i,
                    idBuf, idBuf.Length, nameBuf, nameBuf.Length, filterBuf, filterBuf.Length)
                != NativeMethods.DcaStatus.Ok) continue;
            var id = Utf8(idBuf);
            if (id.Length == 0) continue;
            list.Add(new JuceDspStringParamInfo(id, Utf8(nameBuf), Utf8(filterBuf)));
        }
        return list;
    }

    /// <summary>Decode a native out-buffer: UTF-8, NUL-terminated, and never the
    /// system codepage (audit #3).</summary>
    internal static string Utf8(byte[] buf)
    {
        var end = Array.IndexOf(buf, (byte)0);
        return Encoding.UTF8.GetString(buf, 0, end < 0 ? buf.Length : end);
    }
}
