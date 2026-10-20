# JuceMixer

`sealed class JuceMixer : IDisposable`

A **managed** multi-source facade with per-channel mute, solo, volume and pan.
Useful for stem-style playback (drums / bass / vocals / …) without needing a
native mixer node.

It owns no native handle and takes no engine: you open the sources yourself and
hand them over.

```csharp
using var engine = new JuceAudioEngine();
using var mixer  = new JuceMixer();

mixer.Add("drums",  engine.OpenFile("drums.wav"),  ownsSource: true);
var vocals = mixer.Add("vocals", engine.OpenFile("vocals.wav"), ownsSource: true);

vocals.Solo = true;   // mutes all others
mixer.Play();
```

Solo on any channel mutes every non-soloed channel; turning all
solos off restores the user mute flags.

## API

| Member | Description |
|--------|-------------|
| `JuceMixer()` | Default constructor — it takes no engine. |
| `JuceMixerChannel Add(string name, JuceAudioSource source, bool ownsSource = false)` | Adds a channel and returns it, folded into the mixer's current solo state — a channel added while another is soloed arrives muted. `ownsSource: true` makes the mixer dispose the source. |
| `bool Remove(JuceMixerChannel channel)` | Removes a channel; disposes its source if the mixer owns it. |
| `IReadOnlyList<JuceMixerChannel> Channels` | Snapshot, taken under lock. |
| `Play() / Pause() / Stop() / Seek(TimeSpan)` | Applied to every channel, muted ones included — mute is a gain (`source_set_volume(0)`), not a transport state, so a muted stem stays in sync. |
| `static bool[] ComputeMuteStates(IReadOnlyList<bool> mutedByUser, IReadOnlyList<bool> solo)` | The pure solo/mute resolution, exposed for testing. |
| `Dispose()` | Disposes any owned sources. |

> ℹ️ **Note**
> There is **no indexer** — `mixer["vocals"]` does not compile. Keep the
> `JuceMixerChannel` that `Add` returns, or find it in `Channels` by `Name`.

## `JuceMixerChannel`

`sealed class JuceMixerChannel`

| Member | Type | Description |
|---|---|---|
| `Name` | `string` | The name passed to `Add`. |
| `Source` | `JuceAudioSource` | The underlying source. |
| `MutedByUser` | `bool` | The user's mute flag; re-resolves the whole mixer's mute states. |
| `Solo` | `bool` | Solo flag; re-resolves the whole mixer's mute states. |
| `Volume` | `float` | Forwards to `Source.Volume`. |
| `Pan` | `float` | Forwards to `Source.Pan`. |
