namespace JuceSharp;

/// <summary>
/// Lightweight scanner for VST3 plugin bundles on disk. Walks the standard
/// host-specific search directories and any user-supplied roots, returning a
/// flat list of plugin file paths. Does NOT instantiate plugins (loading is
/// the caller's job through <see cref="JuceAudioEngine.LoadPlugin(string)"/>).
/// </summary>
public static class JuceVst3Scanner
{
    /// <summary>
    ///     One plugin bundle on disk.
    ///     <para>
    ///         There is deliberately no component COUNT here. A bundle may declare
    ///         several plugins (ABI v33,
    ///         <see cref="JuceAudioEngine.CountVst3Components" />), but counting them
    ///         means asking JUCE to SCAN the bundle — seconds apiece on a cold cache,
    ///         once per entry — and this type's whole contract is that a scan is a
    ///         directory walk that touches no plugin code. Ask for the count on the
    ///         one bundle the user picked.
    ///     </para>
    /// </summary>
    public sealed record Vst3Entry(string Name, string Path, long SizeBytes, DateTime LastWriteUtc)
    {
        /// <summary>The bundle's name with its size in MiB appended, ready to put in a
        /// picker list. Derived from <see cref="Name"/> and <see cref="SizeBytes"/>; no
        /// plugin code is touched to produce it.</summary>
        public string Display => $"{Name}  ({(SizeBytes / (1024d * 1024)):F1} MB)";
    }

    /// <summary>Default VST3 paths for the current OS.</summary>
    public static IReadOnlyList<string> DefaultPaths()
    {
        var list = new List<string>();
        if (OperatingSystem.IsWindows())
        {
            list.Add(@"C:\Program Files\Common Files\VST3");
            list.Add(@"C:\Program Files (x86)\Common Files\VST3");
            var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            if (!string.IsNullOrEmpty(local))
                list.Add(System.IO.Path.Combine(local, "Programs", "Common", "VST3"));
        }
        else if (OperatingSystem.IsMacOS())
        {
            list.Add("/Library/Audio/Plug-Ins/VST3");
            list.Add(System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library/Audio/Plug-Ins/VST3"));
        }
        else if (OperatingSystem.IsLinux())
        {
            list.Add("/usr/lib/vst3");
            list.Add("/usr/local/lib/vst3");
            var home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (!string.IsNullOrEmpty(home))
                list.Add(System.IO.Path.Combine(home, ".vst3"));
        }
        return list;
    }

    /// <summary>Scan the supplied roots (or the OS defaults when null) for
    /// .vst3 bundles. Walks at most <paramref name="maxDepth"/> directory
    /// levels and returns at most <paramref name="limit"/> entries (0 = no
    /// limit). Inaccessible folders are skipped and logged at Debug.
    /// <para><b>This BLOCKS</b>, for seconds on a plugin-heavy machine —
    /// <c>C:\Program Files\Common Files\VST3</c> to depth 4, plus a recursive
    /// byte-sum over every macOS-style bundle. Do not call it on a UI thread;
    /// use <see cref="ScanAsync" />.</para></summary>
    public static IReadOnlyList<Vst3Entry> Scan(IEnumerable<string>? roots = null, int maxDepth = 4, int limit = 0)
        => Scan(roots, maxDepth, limit, CancellationToken.None);

    /// <summary>
    ///     <see cref="Scan(IEnumerable{string},int,int)" /> with cancellation.
    ///     The token is checked per directory entry and inside the bundle
    ///     byte-sum, so a "Scan for plugins" button can be abandoned mid-walk
    ///     rather than only between roots.
    /// </summary>
    /// <exception cref="OperationCanceledException"><paramref name="cancellationToken" />
    /// was cancelled.</exception>
    public static IReadOnlyList<Vst3Entry> Scan(
        IEnumerable<string>? roots, int maxDepth, int limit, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var results = new List<Vst3Entry>();
        foreach (var root in (roots ?? DefaultPaths()))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (string.IsNullOrWhiteSpace(root) || !System.IO.Directory.Exists(root)) continue;
            // OperationCanceledException must NOT be swallowed by the catch that
            // exists for permission errors — a cancelled scan that returns a
            // partial list looks exactly like a machine with few plugins.
            try { Walk(root, 0); }
            catch (OperationCanceledException) { throw; }
            catch (Exception ex) { Skipped(root, ex); }
            if (limit > 0 && results.Count >= limit) break;
        }
        return results;

        void Walk(string dir, int depth)
        {
            if (limit > 0 && results.Count >= limit) return;
            cancellationToken.ThrowIfCancellationRequested();
            string[] entries;
            try { entries = System.IO.Directory.GetFileSystemEntries(dir); }
            catch (Exception ex) { Skipped(dir, ex); return; }

            foreach (var e in entries)
            {
                if (limit > 0 && results.Count >= limit) return;
                cancellationToken.ThrowIfCancellationRequested();
                try
                {
                    if (e.EndsWith(".vst3", StringComparison.OrdinalIgnoreCase) && seen.Add(e))
                    {
                        // On Windows VST3 is usually a single file, on macOS a
                        // bundle directory. Treat both uniformly.
                        var info = new System.IO.FileInfo(e);
                        long size;
                        DateTime mtime;
                        if (info.Exists) { size = info.Length; mtime = info.LastWriteTimeUtc; }
                        else
                        {
                            var dinfo = new System.IO.DirectoryInfo(e);
                            size  = DirectorySize(dinfo, cancellationToken);
                            mtime = dinfo.LastWriteTimeUtc;
                        }
                        var name = System.IO.Path.GetFileNameWithoutExtension(e);
                        results.Add(new Vst3Entry(name, e, size, mtime));
                        continue;
                    }
                    if (depth < maxDepth && System.IO.Directory.Exists(e))
                        Walk(e, depth + 1);
                }
                catch (OperationCanceledException) { throw; }
                catch (Exception ex) { Skipped(e, ex); }
            }
        }

        static long DirectorySize(System.IO.DirectoryInfo d, CancellationToken ct)
        {
            try
            {
                long total = 0;
                foreach (var f in d.EnumerateFiles("*", System.IO.SearchOption.AllDirectories))
                {
                    ct.ThrowIfCancellationRequested();
                    total += f.Length;
                }
                return total;
            }
            catch (OperationCanceledException) { throw; }
            catch (Exception ex) { Skipped(d.FullName, ex); return 0; }
        }
    }

    /// <summary>
    ///     <see cref="Scan(IEnumerable{string},int,int,CancellationToken)" /> on a
    ///     dedicated background thread, so a "Scan for plugins" button does not
    ///     block the UI and does not park a thread-pool thread for the seconds
    ///     this takes — the rule <c>Interop/EngineWorker</c> states in full. It
    ///     was the last blocking public member in the wrapper with no async twin
    ///     (W4-E3).
    /// </summary>
    public static Task<IReadOnlyList<Vst3Entry>> ScanAsync(
        IEnumerable<string>? roots = null,
        int maxDepth = 4,
        int limit = 0,
        CancellationToken cancellationToken = default)
    {
        if (cancellationToken.IsCancellationRequested)
            return Task.FromCanceled<IReadOnlyList<Vst3Entry>>(cancellationToken);

        // Materialise here: the caller's enumerable may not be thread-safe, and
        // the walk runs on another thread.
        var snapshot = roots?.ToArray();

        var tcs = new TaskCompletionSource<IReadOnlyList<Vst3Entry>>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var thread = new Thread(() =>
        {
            try { tcs.TrySetResult(Scan(snapshot, maxDepth, limit, cancellationToken)); }
            catch (OperationCanceledException oce) { tcs.TrySetCanceled(oce.CancellationToken); }
            catch (Exception ex) { tcs.TrySetException(ex); }
        })
        {
            IsBackground = true,
            Name = "JuceVst3Scanner",
        };
        thread.Start();
        return tcs.Task;
    }

    /// <summary>
    /// A skipped root, folder or entry, at Debug. The three bare
    /// <c>catch { }</c> blocks this replaces made a permission-denied root and a
    /// corrupt bundle indistinguishable from "nothing there" — which is the
    /// shape of every "the scanner does not find my plugins" report nobody can
    /// reproduce.
    /// </summary>
    private static void Skipped(string path, Exception ex)
        => JuceLog.Emit(JuceLogLevel.Debug,
            $"VST3 scan skipped '{path}': {ex.GetType().Name}: {ex.Message}");
}
