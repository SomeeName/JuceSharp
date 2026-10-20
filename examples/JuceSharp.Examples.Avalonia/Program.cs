using System;
using System.Linq;
using Avalonia;

namespace JuceSharp.Examples.AvaloniaDemo;

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        // A GUI app still owes the shell an answer. This is a WinExe with no console of its own, so
        // the text only appears when stdout is redirected (`… --help | cat`, or any script that
        // captures it) — which is exactly the case that would otherwise get a silently opened window.
        if (args.Any(a => a is "-h" or "--help" or "/?"))
        {
            PrintHelp();
            return 0;
        }
        return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    private static void PrintHelp()
    {
        // ASCII only, deliberately: a WinExe has no console of its own, so it never gets the UTF-8
        // code page a console app can set, and an em-dash here comes out as mojibake in a pipe.
        Console.WriteLine("JuceSharp.Examples.Avalonia - desktop demo of the whole managed JuceSharp surface.");
        Console.WriteLine();
        Console.WriteLine("Usage:");
        Console.WriteLine("  JuceSharp.Examples.Avalonia [-h|--help]");
        Console.WriteLine();
        Console.WriteLine("Takes no arguments: everything is chosen in the window - driver and device, sample");
        Console.WriteLine("rate and buffer size, the file to play, the VST3 to host, the tone rig to build.");
        Console.WriteLine();
        Console.WriteLine("No audio device is opened until you press \"Start engine\", so it launches on a");
        Console.WriteLine("machine with no sound card. To drive the engine headlessly, set");
        Console.WriteLine("DCA_ENABLE_NULL_DEVICE=1 before launching and pick the \"Null\" driver in the");
        Console.WriteLine("driver dropdown.");
    }

    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}
