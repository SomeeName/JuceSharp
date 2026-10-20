using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace JuceSharp.Examples.AvaloniaDemo.Controls;

/// <summary>
/// Spectrogram heat-map renderer. Set <see cref="Spectrogram"/> to a
/// <c>float[frame][bin]</c> matrix in dB (values typically -100..0) and
/// the control regenerates a <c>WriteableBitmap</c>, scaled to fit. A
/// playhead line tracks <see cref="PositionRatio"/> in [0,1].
/// </summary>
public sealed class SpectrogramDisplay : Control
{
    public static readonly StyledProperty<float[][]?> SpectrogramProperty =
        AvaloniaProperty.Register<SpectrogramDisplay, float[][]?>(nameof(Spectrogram));

    public static readonly StyledProperty<double> PositionRatioProperty =
        AvaloniaProperty.Register<SpectrogramDisplay, double>(nameof(PositionRatio));

    public static readonly StyledProperty<int> MaxBinProperty =
        AvaloniaProperty.Register<SpectrogramDisplay, int>(nameof(MaxBin), 256);

    public float[][]? Spectrogram { get => GetValue(SpectrogramProperty); set => SetValue(SpectrogramProperty, value); }
    public double    PositionRatio { get => GetValue(PositionRatioProperty); set => SetValue(PositionRatioProperty, value); }
    public int       MaxBin       { get => GetValue(MaxBinProperty);       set => SetValue(MaxBinProperty,       value); }

    private WriteableBitmap? _bitmap;
    private float[][]? _lastData;

    static SpectrogramDisplay()
    {
        AffectsRender<SpectrogramDisplay>(SpectrogramProperty, PositionRatioProperty, MaxBinProperty);
    }

    public override void Render(DrawingContext ctx)
    {
        var w = Bounds.Width;
        var h = Bounds.Height;
        if (w <= 1 || h <= 1) return;

        ctx.FillRectangle(new SolidColorBrush(Color.FromRgb(16, 18, 22)), new Rect(0, 0, w, h));

        var data = Spectrogram;
        if (data is null || data.Length == 0 || data[0] is null || data[0].Length == 0)
        {
            var ft = new FormattedText("Run analysis to render a spectrogram.",
                System.Globalization.CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight,
                Typeface.Default, 13, new SolidColorBrush(Color.FromRgb(120, 124, 132)));
            ctx.DrawText(ft, new Point(12, h / 2.0 - ft.Height / 2.0));
            return;
        }

        if (!ReferenceEquals(_lastData, data) || _bitmap is null)
        {
            _bitmap   = BuildBitmap(data);
            _lastData = data;
        }

        if (_bitmap is not null)
        {
            ctx.DrawImage(_bitmap, new Rect(0, 0, _bitmap.PixelSize.Width, _bitmap.PixelSize.Height), new Rect(0, 0, w, h));
        }

        var ratio = System.Math.Clamp(PositionRatio, 0.0, 1.0);
        if (ratio > 0)
        {
            var x = ratio * w;
            ctx.DrawLine(new Pen(new SolidColorBrush(Color.FromRgb(255, 196, 0)), 2.0),
                new Point(x, 0), new Point(x, h));
        }
    }

    private WriteableBitmap BuildBitmap(float[][] data)
    {
        var frames = data.Length;
        var bins   = System.Math.Min(MaxBin <= 0 ? data[0].Length : MaxBin, data[0].Length);
        var bmp    = new WriteableBitmap(new PixelSize(frames, bins), new Vector(96, 96),
                                         PixelFormat.Bgra8888, AlphaFormat.Premul);

        using var fb = bmp.Lock();
        unsafe
        {
            var ptr    = (byte*)fb.Address.ToPointer();
            var stride = fb.RowBytes;
            for (var y = 0; y < bins; y++)
            {
                var row    = ptr + (bins - 1 - y) * stride;
                for (var x = 0; x < frames; x++)
                {
                    var v = data[x][y];
                    var t = System.Math.Clamp((v + 80f) / 80f, 0f, 1f);
                    Colorise(t, out var r, out var g, out var b);
                    var off = x * 4;
                    row[off + 0] = b;
                    row[off + 1] = g;
                    row[off + 2] = r;
                    row[off + 3] = 255;
                }
            }
        }
        return bmp;
    }

    private static void Colorise(float t, out byte r, out byte g, out byte b)
    {
        // Black → blue → magenta → yellow → white (inferno-like).
        var stops = new (float t, byte r, byte g, byte b)[]
        {
            (0.00f,  0,   0,   0),
            (0.20f, 12,   8,  82),
            (0.40f,107,   0, 134),
            (0.60f,206,  68,  62),
            (0.80f,252, 188,  37),
            (1.00f,255, 255, 240),
        };
        for (var i = 1; i < stops.Length; i++)
        {
            if (t <= stops[i].t)
            {
                var span = stops[i].t - stops[i - 1].t;
                var k    = span == 0 ? 0f : (t - stops[i - 1].t) / span;
                r = (byte)System.Math.Round(stops[i - 1].r + (stops[i].r - stops[i - 1].r) * k);
                g = (byte)System.Math.Round(stops[i - 1].g + (stops[i].g - stops[i - 1].g) * k);
                b = (byte)System.Math.Round(stops[i - 1].b + (stops[i].b - stops[i - 1].b) * k);
                return;
            }
        }
        r = g = b = 255;
    }
}
