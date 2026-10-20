// Tone rig tab (ABI v24-v32) + dropout counters (ABI v30) for the Avalonia showcase.
//
// Everything here goes through the JuceSharp wrapper: JuceDspModuleCatalog for the
// palette, engine.GetDspChain(seat) for the rig, JuceDspNode for a pedal. This file
// used to bind the C ABI itself through a linked DspInterop.cs copied from the
// console example, because the package exposed none of the rig; that file is gone.
//
// The wrapper also buys lifetime safety this tab could not have before: every call
// pins the engine handle, so a rig poll landing while the engine is being replaced
// is a caught ObjectDisposedException rather than a read through freed memory —
// which is exactly what the 100 ms refresh timer below used to risk.

using System;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using JuceSharp.Dsp;

namespace JuceSharp.Examples.AvaloniaDemo.ViewModels;

public sealed partial class MainWindowViewModel
{
    private const int RigSeat = 0;
    private DispatcherTimer? _rigTimer;
    private int _rigTick;
    private JuceDspNodeMeter[] _rigMeterScratch = Array.Empty<JuceDspNodeMeter>();

    public ObservableCollection<RigModuleItem> RigModules { get; } = new();
    public ObservableCollection<RigNodeItem>   RigNodes   { get; } = new();
    public ObservableCollection<RigParam>      RigParams  { get; } = new();

    [ObservableProperty] private RigModuleItem? _selectedRigModule;
    [ObservableProperty] private RigNodeItem?   _selectedRigNode;
    [ObservableProperty] private string _rigStatus = "(engine not running)";
    [ObservableProperty] private int    _rigLatencySamples;
    [ObservableProperty] private string _dropoutStatus = "dropouts: n/a";

    private void InitToneRig()
    {
        try
        {
            // Static catalogue: no engine, no device, so the palette is populated
            // before anything is opened.
            foreach (var m in JuceDspModuleCatalog.Modules) RigModules.Add(new RigModuleItem(m));
            SelectedRigModule = RigModules.FirstOrDefault(m => m.Type.Id == "overdrive") ?? RigModules.FirstOrDefault();
            AppendLog($"DSP catalogue: {RigModules.Count} module types.");
        }
        catch (Exception ex) { AppendLog($"DSP catalogue unavailable: {ex.Message}"); }

        _rigTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
        _rigTimer.Tick += (_, _) => RefreshRig();
        _rigTimer.Start();
    }

    private void OnEngineReplacedForRig()
    {
        RigNodes.Clear();
        RigParams.Clear();
        SelectedRigNode = null;
        RigStatus = _engine is null ? "(engine not running)" : "empty chain — add a module";
        // v30 meters are OFF until asked for; while off the DSP callback is
        // byte-for-byte the pre-v30 path.
        try { if (Rig is { } chain) chain.MeteringEnabled = true; } catch (Exception ex) { AppendLog($"Metering: {ex.Message}"); }
    }

    /// <summary>This engine's seat-0 rig, or null while no engine is running.</summary>
    private JuceDspChain? Rig
    {
        get { try { return _engine?.GetDspChain(RigSeat); } catch (ObjectDisposedException) { return null; } }
    }

    [RelayCommand] private void AddRigNode()       => AddRigNodeCore(parked: false);
    [RelayCommand] private void AddRigNodeParked() => AddRigNodeCore(parked: true);

    private void AddRigNodeCore(bool parked)
    {
        if (Rig is not { } chain || SelectedRigModule is null) { AppendLog("Start the engine and pick a module first."); return; }
        try
        {
            var type = SelectedRigModule.Type;
            // v32: a parked node is appended at ZERO wet — configure it, then un-bypass to fade it in.
            var node = parked ? chain.AddNodeBypassed(type.Id) : chain.AddNode(type.Id);
            var item = new RigNodeItem(this, node, type);
            RigNodes.Add(item);
            SelectedRigNode = item;
            AppendLog($"Added {type.DisplayName} as node {node.Id}{(parked ? " (parked — set params, then un-bypass)" : "")}.");
        }
        catch (Exception ex) { AppendLog($"Add failed: {ex.Message}"); }
    }

    [RelayCommand]
    private void RemoveRigNode()
    {
        if (Rig is null || SelectedRigNode is null) return;
        try
        {
            SelectedRigNode.Node.Remove(); // fades out, then retires the module
            RigNodes.Remove(SelectedRigNode);
            SelectedRigNode = RigNodes.LastOrDefault();
        }
        catch (Exception ex) { AppendLog($"Remove failed: {ex.Message}"); }
    }

    [RelayCommand] private void MoveRigNodeUp()   => MoveRigNode(-1);
    [RelayCommand] private void MoveRigNodeDown() => MoveRigNode(+1);

    private void MoveRigNode(int delta)
    {
        if (Rig is null || SelectedRigNode is null) return;
        var from = RigNodes.IndexOf(SelectedRigNode);
        var to = from + delta;
        if (from < 0 || to < 0 || to >= RigNodes.Count) return;
        try
        {
            SelectedRigNode.Node.MoveTo(to); // a reorder never clears a delay's buffer
            RigNodes.Move(from, to);
        }
        catch (Exception ex) { AppendLog($"Move failed: {ex.Message}"); }
    }

    [RelayCommand]
    private void ClearRig()
    {
        try { Rig?.Clear(); } catch (Exception ex) { AppendLog($"Clear failed: {ex.Message}"); }
        RigNodes.Clear();
        RigParams.Clear();
        SelectedRigNode = null;
    }

    [RelayCommand]
    private void ResetXrunStats()
    {
        try { _engine?.ResetXrunStats(); AppendLog("Dropout counters reset."); } catch (Exception ex) { AppendLog($"Reset failed: {ex.Message}"); }
    }

    partial void OnSelectedRigNodeChanged(RigNodeItem? value)
    {
        RigParams.Clear();
        if (value is null || Rig is null) return;
        try
        {
            foreach (var p in value.Type.Params)
                RigParams.Add(new RigParam(this, value.Node, p, value.Node.GetParam(p.Id)));
        }
        catch (Exception ex) { AppendLog($"Params: {ex.Message}"); }
    }

    internal void ReportRigFailure(string what, Exception ex) => AppendLog($"{what}: {ex.Message}");

    private void RefreshRig()
    {
        if (Rig is not { } chain) return;
        try
        {
            if (RigNodes.Count > 0)
            {
                // One P/Invoke per frame for every node's meter, in chain order.
                if (_rigMeterScratch.Length < RigNodes.Count) _rigMeterScratch = new JuceDspNodeMeter[RigNodes.Count];
                chain.TryReadMeters(_rigMeterScratch, out var count);
                for (var i = 0; i < Math.Min(count, RigNodes.Count); i++)
                {
                    RigNodes[i].UpdateMeter(_rigMeterScratch[i].InputPeak, _rigMeterScratch[i].OutputPeak,
                        RigNodes[i].Node.LatencySamples);
                }
                RigLatencySamples = chain.LatencySamples;
                RigStatus = $"{RigNodes.Count} node(s), chain latency {RigLatencySamples} samples ({RigLatencySamples * 1000.0 / Math.Max(1, _engine!.SampleRate):F1} ms)";
            }

            if (++_rigTick % 5 == 0 && _engine!.TryGetXrunStats(out var x))
            {
                DropoutStatus = $"callbacks {x.Callbacks}  late {x.LateCallbacks}  overruns {x.DeadlineOverruns}  near-miss {x.NearMisses}  " +
                                $"driver {(x.DeviceXruns?.ToString(CultureInfo.InvariantCulture) ?? "n/a")}  load {x.Load:P0}";
            }
        }
        catch (ObjectDisposedException) { /* engine being torn down under the timer — exactly what the pinning turns into this */ }
    }

    private void DisposeToneRig() => _rigTimer?.Stop();

    // ------------------------------------------------------------------ items

    public sealed class RigModuleItem
    {
        internal RigModuleItem(JuceDspModuleInfo type) { Type = type; }
        internal JuceDspModuleInfo Type { get; }
        // Type.Category (ABI v33) is the ENGINE's own word out of a fixed set —
        // a real palette would group the dropdown by it.
        public string Display => $"[{Type.Category}] {Type.DisplayName}  ({Type.Id}, {Type.Params.Count} params)";
    }

    public sealed partial class RigNodeItem : ObservableObject
    {
        private readonly MainWindowViewModel _vm;
        internal JuceDspNode Node { get; }
        internal JuceDspModuleInfo Type { get; }
        public int Id => Node.Id;
        public string Display => $"{Type.DisplayName}  #{Node.Id}  [{Node.TypeId}/{Type.Category}]";

        [ObservableProperty] private bool   _bypassed;
        [ObservableProperty] private string _meterText = "—";
        [ObservableProperty] private double _inLevel;   // 0..1 for a bar
        [ObservableProperty] private double _outLevel;

        internal RigNodeItem(MainWindowViewModel vm, JuceDspNode node, JuceDspModuleInfo type)
        {
            _vm = vm; Node = node; Type = type; _bypassed = node.Bypass;
        }

        partial void OnBypassedChanged(bool value)
        {
            try { Node.Bypass = value; }
            catch (Exception ex) { _vm.ReportRigFailure("Bypass", ex); }
        }

        public void UpdateMeter(float inPeak, float outPeak, int latency)
        {
            InLevel   = Math.Clamp(inPeak, 0, 1);
            OutLevel  = Math.Clamp(outPeak, 0, 1);
            MeterText = $"{Db(inPeak),6:F1} → {Db(outPeak),6:F1} dBFS   {latency} smp";
        }

        private static double Db(float v) => 20.0 * Math.Log10(Math.Max(v, 1e-5f));
    }

    public sealed partial class RigParam : ObservableObject
    {
        private readonly MainWindowViewModel _vm;
        private readonly JuceDspNode _node;
        internal JuceDspParamInfo Info { get; }
        public string Name => Info.Name;
        public double Min => Info.Min;
        public double Max => Info.Max;
        // Named params step by 1 (value = Min + label index); ordinal ones by their step; knobs are free.
        public double Step => Info.IsNamed ? 1 : Info.IsOrdinal ? Info.Step : 0;

        [ObservableProperty] private float _value;
        [ObservableProperty] private string _display = string.Empty;

        internal RigParam(MainWindowViewModel vm, JuceDspNode node, JuceDspParamInfo info, float value)
        {
            _vm = vm; _node = node; Info = info; _value = value; _display = info.Describe(value);
        }

        partial void OnValueChanged(float value)
        {
            Display = Info.Describe(value);
            try { _node.SetParam(Info.Id, value); }
            catch (Exception ex) { _vm.ReportRigFailure($"SetParam {Info.Id}", ex); }
        }
    }
}
