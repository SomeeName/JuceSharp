using System.Threading;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Platform.Storage;
using JuceSharp.Examples.AvaloniaDemo.ViewModels;

namespace JuceSharp.Examples.AvaloniaDemo.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
    }

    private void InitializeComponent() => AvaloniaXamlLoader.Load(this);

    // The handlers below are `void` and hand their Task to Forget(), rather than being `async void`.
    // An `async void` that throws does not unwind into a catch anywhere — it is raised on the
    // synchronisation context and kills the process. Forget() gives the Task somewhere to report.
    private void OnOpenFile(object? sender, RoutedEventArgs e) => Forget(PickAndOpenFileAsync());
    private void OnLoadPlugin(object? sender, RoutedEventArgs e) => Forget(PickAndLoadPluginAsync());

    private void Forget(Task task) => task.ContinueWith(
        t => (DataContext as MainWindowViewModel)?.AppendUserLog($"Unhandled: {t.Exception?.GetBaseException().Message}"),
        CancellationToken.None,
        TaskContinuationOptions.OnlyOnFaulted,
        TaskScheduler.FromCurrentSynchronizationContext());

    private async Task PickAndOpenFileAsync()
    {
        var top = TopLevel.GetTopLevel(this);
        if (top is null) return;
        var picks = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Pick an audio file (WAV / FLAC / OGG / MP3 / AIFF)",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("Audio files")
                {
                    Patterns = new[] { "*.wav", "*.flac", "*.ogg", "*.mp3", "*.aiff", "*.aif" }
                }
            }
        });
        if (picks.Count == 0) return;
        var path = picks[0].TryGetLocalPath();
        if (!string.IsNullOrEmpty(path) && DataContext is MainWindowViewModel vm)
            await vm.OpenFileAsync(path);
    }

    private async Task PickAndLoadPluginAsync()
    {
        var top = TopLevel.GetTopLevel(this);
        if (top is null) return;
        var picks = await top.StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Pick a VST3 plugin",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new FilePickerFileType("VST3 plugins")
                {
                    Patterns = new[] { "*.vst3" }
                }
            }
        });
        if (picks.Count == 0) return;
        var path = picks[0].TryGetLocalPath();
        if (!string.IsNullOrEmpty(path) && DataContext is MainWindowViewModel vm)
            await vm.LoadPluginFromPathAsync(path);
    }

    private void OnSeekReleased(object? sender, PointerReleasedEventArgs e)
    {
        if (sender is Slider s && DataContext is MainWindowViewModel vm)
            vm.SeekTo(s.Value);
    }

    private void OnLoadPluginFromList(object? sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: string path } && DataContext is MainWindowViewModel vm)
            Forget(vm.LoadPluginFromPathAsync(path));
    }
}
