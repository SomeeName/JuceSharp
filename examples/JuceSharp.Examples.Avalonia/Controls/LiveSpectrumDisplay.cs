using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace JuceSharp.Examples.AvaloniaDemo.Controls;

/// <summary>
/// Live FFT magnitude bar plot. Magnitudes are expected in [0,1] (already
/// normalised by the caller). Draws a smooth gradient and a peak-hold line.
/// </summary>
public sealed class LiveSpectrumDisplay : Control
{
    public static readonly StyledProperty<float[]?> MagnitudesProperty =
        AvaloniaProperty.Register<LiveSpectrumDisplay, float[]?>(nameof(Magnitudes));

    public static readonly StyledProperty<float[]?> PeakHoldProperty =
        AvaloniaProperty.Register<LiveSpectrumDisplay, float[]?>(nameof(PeakHold));

    public float[]? Magnitudes { get => GetValue(MagnitudesProperty); set => SetValue(MagnitudesProperty, value); }
    public float[]? PeakHold   { get => GetValue(PeakHoldProperty);   set => SetValue(PeakHoldProperty,   value); }

    static LiveSpectrumDisplay()
    {
        AffectsRender<LiveSpectrumDisplay>(MagnitudesProperty, PeakHoldProperty);
    }

    public override void Render(DrawingContext ctx)
    {
        var w = Bounds.Width;
        var h = Bounds.Height;
        if (w <= 1 || h <= 1) return;

        ctx.FillRectangle(new SolidColorBrush(Color.FromRgb(20, 22, 26)), new Rect(0, 0, w, h));

        var mags = Magnitudes;
        if (mags is null || mags.Length == 0)
        {
            var ft = new FormattedText("Live spectrum — load a file and press ▶",
                System.Globalization.CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight,
                Typeface.Default, 13, new SolidColorBrush(Color.FromRgb(120, 124, 132)));
            ctx.DrawText(ft, new Point(12, h / 2.0 - ft.Height / 2.0));
            return;
        }

        var n      = mags.Length;
        var barW   = w / n;
        var grad   = new LinearGradientBrush
        {
            StartPoint = new RelativePoint(0, 1, RelativeUnit.Relative),
            EndPoint   = new RelativePoint(0, 0, RelativeUnit.Relative),
            GradientStops =
            {
                new GradientStop(Color.FromRgb(0x2A, 0x6C, 0xF4), 0.0),
                new GradientStop(Color.FromRgb(0x46, 0xE2, 0xA8), 0.6),
                new GradientStop(Color.FromRgb(0xFF, 0xC4, 0x00), 1.0),
            }
        };

        for (var i = 0; i < n; i++)
        {
            var v = System.Math.Clamp(mags[i], 0f, 1f);
            var bh = v * h;
            ctx.FillRectangle(grad, new Rect(i * barW, h - bh, System.Math.Max(1, barW - 1), bh));
        }

        var peaks = PeakHold;
        if (peaks is not null && peaks.Length == n)
        {
            var pen = new Pen(new SolidColorBrush(Color.FromArgb(180, 255, 255, 255)), 1.0);
            for (var i = 0; i < n; i++)
            {
                var v = System.Math.Clamp(peaks[i], 0f, 1f);
                var y = h - v * h;
                ctx.DrawLine(pen, new Point(i * barW, y), new Point(i * barW + barW - 1, y));
            }
        }
    }
}
