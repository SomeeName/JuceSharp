using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using JuceSharp.Interop;
using Xunit;

namespace JuceSharp.Tests;

/// <summary>
/// Every managed delegate handed to native is a reverse-P/Invoke, so an
/// exception out of it unwinds through C++ frames: a process kill, not a
/// catchable error. Three of the wrapper's four callbacks got this right from
/// the start; <c>JuceStemSeparation</c>'s progress lambda did not, and it is
/// invoked from a demucs worker thread while a progress bar updates — exactly
/// the shape that throws.
/// <para>
/// The lint below is the half that cannot be forgotten next time: it discovers
/// the <c>[UnmanagedFunctionPointer]</c> delegate types from
/// <c>NativeMethods.cs</c> and requires a catch-all in the body of everything
/// assigned to one.
/// </para>
/// </summary>
public sealed class NativeCallbackSafetyTests
{
    /// <summary>
    /// The behavioural half. A throwing host handler must be swallowed and
    /// logged rather than allowed to escape the delegate. Invoked directly —
    /// the point is the delegate's own body, not the native call around it.
    /// </summary>
    [Fact]
    public void AThrowingStemProgressHandler_IsSwallowedAndLogged()
    {
        var messages = new List<string>();
        void Capture(JuceLogLevel level, string msg)
        {
            if (level >= JuceLogLevel.Warn) lock (messages) messages.Add(msg);
        }

        JuceLog.MessageReceived += Capture;
        try
        {
            var cb = JuceStemSeparation.WrapProgress((_, _) => throw new InvalidOperationException("boom"));
            Assert.NotNull(cb);
            // Must NOT propagate. Before the fix this threw straight back out —
            // which, from a native worker thread, is a process kill.
            cb!(nint.Zero, 0.5f, "separating");
        }
        finally { JuceLog.MessageReceived -= Capture; }

        lock (messages)
        {
            Assert.Contains(messages, m =>
                m.Contains("progress handler threw", StringComparison.OrdinalIgnoreCase) &&
                m.Contains("InvalidOperationException", StringComparison.Ordinal));
        }
    }

    /// <summary>A non-throwing handler still receives its arguments unchanged —
    /// so the catch-all is a guard, not a swallow of the whole callback.</summary>
    [Fact]
    public void ANormalStemProgressHandler_StillSeesItsArguments()
    {
        float? seenFraction = null;
        string? seenStage = null;

        var cb = JuceStemSeparation.WrapProgress((f, s) => { seenFraction = f; seenStage = s; });
        Assert.NotNull(cb);
        cb!(nint.Zero, 0.25f, "demucs");

        Assert.Equal(0.25f, seenFraction);
        Assert.Equal("demucs", seenStage);
    }

    /// <summary>No handler ⇒ no delegate to marshal, which is what lets
    /// <c>Separate</c> pass a null callback to native.</summary>
    [Fact]
    public void NoProgressHandler_MarshalsNoDelegateAtAll()
        => Assert.Null(JuceStemSeparation.WrapProgress(null));

    /// <summary>
    /// THE lint. Discovers every <c>[UnmanagedFunctionPointer]</c> delegate type
    /// declared in <c>NativeMethods.cs</c>, finds every variable/field of one of
    /// those types anywhere else in the wrapper, and requires that whatever is
    /// assigned to it — a lambda body or a named method — contains a
    /// <c>catch</c>. Comments are stripped first: a <c>// never let a host
    /// exception escape</c> in prose is exactly as inert as no guard at all.
    /// </summary>
    [Fact]
    public void EveryDelegateHandedToNative_HasACatchAllBody()
    {
        var wrapperDir = FindWrapperSourceDir();
        var nativeMethods = Path.Combine(wrapperDir, "Interop", "NativeMethods.cs");
        Assert.True(File.Exists(nativeMethods), $"NativeMethods.cs not found at {nativeMethods}");

        var delegateNames = Regex.Matches(
                StripComments(File.ReadAllText(nativeMethods)),
                @"\[UnmanagedFunctionPointer[^\]]*\]\s*internal\s+delegate\s+\w+\s+(\w+)\s*\(")
            .Select(m => m.Groups[1].Value)
            .Distinct()
            .ToArray();

        Assert.True(delegateNames.Length >= 4,
            "expected at least four [UnmanagedFunctionPointer] delegates in NativeMethods.cs; found "
            + $"{delegateNames.Length}. This lint is scanning the wrong file or the declarations "
            + "moved — repoint it, do not delete it.");

        var alternation = string.Join("|", delegateNames.Select(Regex.Escape));
        var checkedSites = 0;
        var failures = new List<string>();

        foreach (var file in Directory.EnumerateFiles(wrapperDir, "*.cs", SearchOption.AllDirectories))
        {
            if (file.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal) ||
                file.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}",
                    StringComparison.Ordinal) ||
                Path.GetFileName(file) == "NativeMethods.cs")
                continue;

            var code = StripComments(File.ReadAllText(file));
            var name = Path.GetFileName(file);

            // Shape 1: a FACTORY — a method whose return type is one of those
            // delegate types. Whatever it hands back is going to native, so the
            // guard has to be somewhere in its body.
            foreach (Match factory in Regex.Matches(code,
                         $@"\b(?:NativeMethods\.)?(?:{alternation})\??\s+(\w+)\s*\([^)]*\)\s*\{{"))
            {
                checkedSites++;
                var body = ExtractBraceBlock(code, factory.Index + factory.Length - 1);
                if (!body.Contains("catch", StringComparison.Ordinal))
                    failures.Add($"{name}: '{factory.Groups[1].Value}' returns a delegate that is "
                                 + "handed to native and its body has NO catch. A throw out of a "
                                 + "reverse-P/Invoke unwinds through C++ frames — that is a process "
                                 + "kill, not an exception. Wrap it like JuceMidiInput.OnMidi.");
            }

            // Shape 2: every identifier declared with one of those delegate types.
            var declared = Regex.Matches(code,
                    $@"\b(?:NativeMethods\.)?(?:{alternation})\??\s+(\w+)\s*[=;]")
                .Select(m => m.Groups[1].Value)
                .Distinct()
                .ToArray();

            foreach (var variable in declared)
            {
                foreach (Match assign in Regex.Matches(code, $@"\b{Regex.Escape(variable)}\s*=\s*(?!=)"))
                {
                    var rhs = code[(assign.Index + assign.Length)..].TrimStart();
                    if (rhs.StartsWith("null", StringComparison.Ordinal)) continue;

                    checkedSites++;
                    var body = ExtractCallbackBody(rhs, code);
                    if (body is null)
                    {
                        failures.Add($"{name}: could not resolve the body assigned to '{variable}'. "
                                     + "The lint has to see the body to check it — simplify the "
                                     + "assignment or extend the lint.");
                        continue;
                    }

                    if (!body.Contains("catch", StringComparison.Ordinal))
                        failures.Add($"{name}: the delegate assigned to '{variable}' is handed to "
                                     + "native and has NO catch. A throw out of a reverse-P/Invoke "
                                     + "unwinds through C++ frames — that is a process kill, not an "
                                     + "exception. Wrap the body like JuceMidiInput.OnMidi.");
                }
            }
        }

        Assert.True(checkedSites >= 4,
            $"the lint only found {checkedSites} delegate assignment sites. There are at least four "
            + "(the log trampoline, the editor-closed thunk, the MIDI callback and the stem progress "
            + "callback) — the scan is broken, not the code.");
        Assert.True(failures.Count == 0, string.Join("\n", failures));
    }

    /// <summary>
    /// Resolve what a delegate assignment actually runs: a lambda's own body, or
    /// — for a method group — the named method's body found in the same file.
    /// </summary>
    private static string? ExtractCallbackBody(string rhs, string wholeFile)
    {
        var lambda = Regex.Match(rhs, @"^\(?[\w\s,_]*\)?\s*=>\s*");
        if (lambda.Success)
        {
            var rest = rhs[lambda.Length..];
            return rest.StartsWith('{')
                ? ExtractBraceBlock(rest, 0)
                // An expression-bodied lambda has no statements and so cannot
                // carry a catch — return it and let the caller fail it.
                : rest[..Math.Min(rest.Length, rest.IndexOf(';') < 0 ? rest.Length : rest.IndexOf(';'))];
        }

        var methodGroup = Regex.Match(rhs, @"^(\w+)\s*[;,)]");
        if (!methodGroup.Success) return null;

        var method = Regex.Match(wholeFile,
            $@"\b{Regex.Escape(methodGroup.Groups[1].Value)}\s*\([^)]*\)\s*\{{");
        return method.Success ? ExtractBraceBlock(wholeFile, method.Index + method.Length - 1) : null;
    }

    private static string ExtractBraceBlock(string text, int openBraceIndex)
    {
        var depth = 0;
        for (var i = openBraceIndex; i < text.Length; i++)
        {
            if (text[i] == '{') depth++;
            else if (text[i] == '}' && --depth == 0) return text[openBraceIndex..(i + 1)];
        }
        return text[openBraceIndex..];
    }

    private static string StripComments(string code)
    {
        code = Regex.Replace(code, @"/\*.*?\*/", " ", RegexOptions.Singleline);
        return Regex.Replace(code, @"//[^\n]*", " ");
    }

    private static string FindWrapperSourceDir()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            var candidate = Path.Combine(dir.FullName, "src", "JuceSharp");
            if (File.Exists(Path.Combine(candidate, "JuceAudioEngine.cs"))) return candidate;
            dir = dir.Parent;
        }

        throw new DirectoryNotFoundException(
            "could not locate src/JuceSharp by walking up from " + AppContext.BaseDirectory);
    }
}
