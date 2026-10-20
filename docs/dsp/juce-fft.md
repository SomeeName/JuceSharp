# JuceFft

In-place radix-2 Cooley-Tukey FFT for power-of-two sizes. Twiddle
tables and bit-reverse indices are precomputed at construction time;
`Forward` is allocation-free.

```csharp
var fft = new JuceFft(1024);
var re  = new float[1024];
var im  = new float[1024];
samples.CopyTo(re);
fft.Forward(re, im);
```

For the common case of a real input → magnitude spectrum:

```csharp
float[] mag = fft.Magnitude(samples);   // length = size / 2
```

## API

| Member | Description |
|--------|-------------|
| `JuceFft(int size)` | Throws `ArgumentException` if `size` is not a power of two ≥ 2. |
| `int Size` | Transform size. |
| `void Forward(Span<float> re, Span<float> im)` | In-place complex FFT. Both spans must equal `Size`. |
| `float[] Magnitude(ReadOnlySpan<float> samples)` | Convenience: returns first half of the magnitude spectrum. |
