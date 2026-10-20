# JuceVst3Scanner

`static class JuceVst3Scanner`

A lightweight scanner for VST3 plugin bundles on disk. It walks the standard
host-specific search directories plus any roots you supply and returns a flat
list of plugin paths.

> ℹ️ **Note**
> **This is a file-system walk, not a plugin scan.** Nothing is loaded,
> instantiated or executed, so it is cheap and it cannot crash on a bad
> plugin. Actually loading one is `JuceAudioEngine.LoadPlugin`, which *can*
> take 25 seconds or more for a large amp sim.

## `Vst3Entry`

`sealed record JuceVst3Scanner.Vst3Entry(string Name, string Path, long SizeBytes, DateTime LastWriteUtc)`

It is **nested inside the static `JuceVst3Scanner` class**, so name it
`JuceVst3Scanner.Vst3Entry` (or just use `var`) — a bare `Vst3Entry` does not
compile.

| Member | Type | Description |
|---|---|---|
| `Name` | `string` | The bundle's file name. |
| `Path` | `string` | Full path — pass this to `LoadPlugin`. |
| `SizeBytes` | `long` | Bundle size. |
| `LastWriteUtc` | `DateTime` | Last write time. |
| `Display` | `string` | `"Name  (12.3 MB)"`, ready for a list box. |

## Members

| Member | Description |
|---|---|
| `static IReadOnlyList<string> DefaultPaths()` | The platform's standard VST3 search directories. |
| `static IReadOnlyList<Vst3Entry> Scan(IEnumerable<string>? roots = null, int maxDepth = 4, int limit = 0)` | Walk `roots` (or `DefaultPaths()` when `null`). `limit = 0` means no cap. **Blocks** — seconds on a plugin-heavy machine. |
| `static IReadOnlyList<Vst3Entry> Scan(IEnumerable<string>? roots, int maxDepth, int limit, CancellationToken ct)` | The same, cancellable per directory entry and inside the bundle byte-sum. Throws `OperationCanceledException` rather than returning a partial list. |
| `static Task<IReadOnlyList<Vst3Entry>> ScanAsync(IEnumerable<string>? roots = null, int maxDepth = 4, int limit = 0, CancellationToken ct = default)` | The walk on a dedicated background thread — not the thread pool. Use this from a "Scan for plugins" button. |

## Example

```csharp
foreach (JuceVst3Scanner.Vst3Entry e in JuceVst3Scanner.Scan())
    Console.WriteLine(e.Display);

// A user-added folder, shallow, capped.
var extra = JuceVst3Scanner.Scan(
    roots: new[] { @"D:\Plugins\VST3" },
    maxDepth: 2,
    limit: 200);

using var amp = engine.LoadPlugin(extra[0].Path);
```

## Notes

* `maxDepth` matters: some vendors nest bundles several folders deep, and a
  user's plugin drive can be enormous. The default of 4 covers the usual
  layouts.
* Use `limit` for a responsive first paint — scan shallow and capped, show the
  list, then rescan wider in the background if you need to.
* `LastWriteUtc` is a cheap cache key. If nothing in the list changed, you do
  not need to re-probe the plugins you have already inspected.
