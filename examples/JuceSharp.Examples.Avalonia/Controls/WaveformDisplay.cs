using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace JuceSharp.Examples.AvaloniaDemo.Controls;

/// <summary>
/// Lightweight min/max waveform renderer that paints the classic
/// double-envelope shape from two parallel float arrays produced by
/// <c>JuceSharp.Dsp.JuceWaveformPeaks</c>. Also draws a playhead.
/// </summary>
public sealed class WaveformDisplay : Control
{
    public static readonly StyledProperty<float[]?> MinsProperty =
        AvaloniaProperty.Register<WaveformDisplay, float[]?>(nameof(Mins));
    public static readonly StyledProperty<float[]?> MaxsProperty =
        AvaloniaProperty.Register<WaveformDisplay, float[]?>(nameof(Maxs));
    public static readonly StyledProperty<double> PositionRatioProperty =
        AvaloniaProperty.Register<WaveformDisplay, double>(nameof(PositionRatio));
    public static readonly StyledProperty<double> LengthSecondsProperty =
        AvaloniaProperty.Register<WaveformDisplay, double>(nameof(LengthSeconds));

    public float[]? Mins         { get => GetValue(MinsProperty);          set => SetValue(MinsProperty, value); }
    public float[]? Maxs         { get => GetValue(MaxsProperty);          set => SetValue(MaxsProperty, value); }
    public double   PositionRatio{ get => GetValue(PositionRatioProperty); set => SetValue(PositionRatioProperty, value); }
    public double   LengthSeconds{ get => GetValue(LengthSecondsProperty); set => SetValue(LengthSecondsProperty, value); }

    static WaveformDisplay()
    {
        AffectsRender<WaveformDisplay>(
            MinsProperty, MaxsProperty, PositionRatioProperty, LengthSecondsProperty);
    }

    public override void Render(DrawingContext ctx)
    {
        var w = Bounds.Width;
        var h = Bounds.Height;
        if (w <= 1 || h <= 1) return;

        ctx.FillRectangle(Brushes.Black, new Rect(0, 0, w, h));

        var midY = h / 2.0;
        ctx.DrawLine(new Pen(new SolidColorBrush(Color.FromRgb(48, 50, 56))), new Point(0, midY), new Point(w, midY));

        var mins = Mins;
        var maxs = Maxs;
        if (mins != null && maxs != null && mins.Length > 0)
        {
            var count = System.Math.Min(mins.Length, maxs.Length);
            var stroke = new Pen(new SolidColorBrush(Color.FromRgb(86, 156, 255)), 1.0);
            for (var i = 0; i < count; i++)
            {
                var x = (i / (double)(count - 1 == 0 ? 1 : count - 1)) * w;
                var y1 = midY - System.Math.Clamp(maxs[i], -1f, 1f) * midY;
                var y2 = midY - System.Math.Clamp(mins[i], -1f, 1f) * midY;
                ctx.DrawLine(stroke, new Point(x, y1), new Point(x, y2));
            }
        }
        else
        {
            var ft = new FormattedText("Waveform appears once a file is loaded.",
                System.Globalization.CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight,
                Typeface.Default, 13, new SolidColorBrush(Color.FromRgb(120, 124, 132)));
            ctx.DrawText(ft, new Point(12, h / 2.0 - ft.Height / 2.0));
        }

        if (LengthSeconds > 0 && PositionRatio >= 0)
        {
            var ratio = System.Math.Clamp(PositionRatio / LengthSeconds, 0.0, 1.0);
            var x = ratio * w;
            ctx.DrawLine(new Pen(new SolidColorBrush(Color.FromRgb(255, 196, 0)), 2.0),
                new Point(x, 0), new Point(x, h));
        }
    }
}
