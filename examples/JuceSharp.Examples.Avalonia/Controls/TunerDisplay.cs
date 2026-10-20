using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace JuceSharp.Examples.AvaloniaDemo.Controls;

/// <summary>
/// Chromatic tuner needle. Set <see cref="Cents"/> in [-50,+50], the note
/// name in <see cref="Note"/>, and (optionally) the detected frequency.
/// Green when within ±5 cents, amber within ±15, red beyond.
/// </summary>
public sealed class TunerDisplay : Control
{
    public static readonly StyledProperty<string?> NoteProperty =
        AvaloniaProperty.Register<TunerDisplay, string?>(nameof(Note));

    public static readonly StyledProperty<double> CentsProperty =
        AvaloniaProperty.Register<TunerDisplay, double>(nameof(Cents));

    public static readonly StyledProperty<double> FrequencyProperty =
        AvaloniaProperty.Register<TunerDisplay, double>(nameof(Frequency));

    public static readonly StyledProperty<bool> HasReadingProperty =
        AvaloniaProperty.Register<TunerDisplay, bool>(nameof(HasReading));

    public string? Note      { get => GetValue(NoteProperty);       set => SetValue(NoteProperty, value); }
    public double  Cents     { get => GetValue(CentsProperty);      set => SetValue(CentsProperty, value); }
    public double  Frequency { get => GetValue(FrequencyProperty);  set => SetValue(FrequencyProperty, value); }
    public bool    HasReading{ get => GetValue(HasReadingProperty); set => SetValue(HasReadingProperty, value); }

    static TunerDisplay()
    {
        AffectsRender<TunerDisplay>(NoteProperty, CentsProperty, FrequencyProperty, HasReadingProperty);
    }

    public override void Render(DrawingContext ctx)
    {
        var w = Bounds.Width;
        var h = Bounds.Height;
        if (w <= 1 || h <= 1) return;

        ctx.FillRectangle(new SolidColorBrush(Color.FromRgb(22, 24, 28)), new Rect(0, 0, w, h));

        // Scale
        var midY  = h * 0.65;
        var span  = w * 0.85;
        var left  = (w - span) / 2;
        var right = left + span;
        var grid  = new Pen(new SolidColorBrush(Color.FromRgb(70, 74, 82)), 1);
        for (var i = -5; i <= 5; i++)
        {
            var x = left + (i + 5) / 10.0 * span;
            var tickH = i == 0 ? 18 : (i % 5 == 0 ? 14 : 8);
            ctx.DrawLine(grid, new Point(x, midY - tickH), new Point(x, midY + tickH));
        }
        ctx.DrawLine(new Pen(new SolidColorBrush(Color.FromRgb(120, 124, 132)), 1.2),
            new Point(left, midY), new Point(right, midY));

        // Note name + frequency
        var noteText = HasReading ? (Note ?? "—") : "—";
        var noteFt = new FormattedText(noteText,
            System.Globalization.CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
            new Typeface("Inter", FontStyle.Normal, FontWeight.Bold), 48,
            new SolidColorBrush(Color.FromRgb(232, 234, 238)));
        ctx.DrawText(noteFt, new Point(w / 2.0 - noteFt.Width / 2.0, 12));

        if (HasReading)
        {
            var freqFt = new FormattedText($"{Frequency:F1} Hz   {Cents:+0.0;-0.0;0.0} ¢",
                System.Globalization.CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                new Typeface("Inter"), 14,
                new SolidColorBrush(Color.FromRgb(160, 166, 174)));
            ctx.DrawText(freqFt, new Point(w / 2.0 - freqFt.Width / 2.0, midY + 24));

            // Needle
            var c     = System.Math.Clamp(Cents, -50, 50);
            var x     = left + (c + 50) / 100.0 * span;
            var abs   = System.Math.Abs(c);
            var color = abs <= 5 ? Color.FromRgb(70, 226, 168)
                      : abs <= 15 ? Color.FromRgb(255, 196, 0)
                      : Color.FromRgb(255, 96, 96);
            ctx.DrawLine(new Pen(new SolidColorBrush(color), 4),
                new Point(x, midY - 26), new Point(x, midY + 26));
        }
    }
}
