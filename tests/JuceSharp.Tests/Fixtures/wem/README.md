# .wem test fixtures

Synthetic tones, generated for this repo (no third-party audio), encoded with
ffmpeg 9.0.1's libvorbis. `Wem/WemFixtureWriter.cs` turns them into Wwise
RIFF Vorbis (.wem) files at test time, in every layout the native decoder
supports (ABI v42).

```sh
ffmpeg -f lavfi -i "aevalsrc=exprs='0.5*sin(2*PI*(220+440*t)*t)+if(lt(mod(t\,0.25)\,0.003)\,0.8\,0)|0.3*sin(2*PI*330*t)+0.2*sin(2*PI*1760*t)*sin(2*PI*3*t)':s=48000:d=1.5" \
       -c:a libvorbis -q:a 2 -map_metadata -1 -fflags +bitexact -flags:a +bitexact stereo48k.ogg
ffmpeg -f lavfi -i "aevalsrc=exprs='0.6*sin(2*PI*440*t)*exp(-3*mod(t\,0.5))':s=44100:d=1.0" \
       -c:a libvorbis -q:a 1 -map_metadata -1 -fflags +bitexact -flags:a +bitexact mono44k.ogg
```

The clicks in `stereo48k.ogg` force short blocks, so the mod-packet window
flags are exercised in both directions.
