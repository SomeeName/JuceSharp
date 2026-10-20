using System.Runtime.InteropServices;

namespace JuceSharp.StemSeparation.Onnx;

/// <summary>
/// One graphics adapter as DXGI enumerates it — the list the DirectML
/// execution provider's <c>device_id</c> indexes into.
/// </summary>
/// <param name="DeviceId">The DXGI adapter index (<c>IDXGIFactory1::EnumAdapters1</c>
/// order), which is exactly the <c>deviceId</c> ONNX Runtime's DirectML provider takes.
/// Index 0 is the adapter driving the primary display, not necessarily the fastest.</param>
/// <param name="Name">The driver's description string, e.g. "NVIDIA GeForce RTX 3080".</param>
/// <param name="VendorId">PCI vendor id: 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel,
/// 0x1414 Microsoft (the software adapter).</param>
/// <param name="DedicatedVideoMemoryBytes">VRAM not shared with the CPU, in bytes; zero or
/// a few hundred MB on an integrated GPU.</param>
/// <param name="SharedSystemMemoryBytes">System memory the adapter may borrow, in bytes.</param>
/// <param name="IsSoftware">True for the WARP / "Microsoft Basic Render Driver" adapter,
/// which DirectML can run on but at CPU speed.</param>
public sealed record OnnxGpuAdapter(
    int DeviceId,
    string Name,
    uint VendorId,
    ulong DedicatedVideoMemoryBytes,
    ulong SharedSystemMemoryBytes,
    bool IsSoftware);

/// <summary>
/// Lists the DXGI adapters on this machine so a caller can choose the DirectML
/// <c>deviceId</c> on purpose instead of taking adapter 0 — the display adapter —
/// by accident. Windows only; anywhere else, and on any DXGI failure, the list is
/// empty, which callers treat as "no GPU to choose".
/// </summary>
/// <remarks>
/// Raw vtable calls rather than a COM interop interface: two methods are needed,
/// the layouts are frozen by the DXGI ABI, and this keeps the assembly free of
/// built-in COM marshalling. Cheap (well under a millisecond) and side-effect free —
/// it creates no D3D device — so it is safe to call from a settings screen.
/// </remarks>
public static class OnnxGpuAdapters
{
    private const uint DxgiAdapterFlagSoftware = 2;
    private const int DxgiErrorNotFound = unchecked((int)0x887A0002);

    // IDXGIFactory1 {770aae78-f26f-4dba-a829-253c83d1b387}
    private static readonly Guid IidDxgiFactory1 = new(0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87);

    // Vtable slots: IUnknown (0-2), IDXGIObject (3-6), IDXGIFactory (7-11),
    // IDXGIFactory1::EnumAdapters1 = 12. IDXGIAdapter (7-9), IDXGIAdapter1::GetDesc1 = 10.
    private const int SlotRelease = 2;
    private const int SlotEnumAdapters1 = 12;
    private const int SlotGetDesc1 = 10;

    /// <summary>Every adapter DXGI reports, in <c>device_id</c> order, the software
    /// adapter included (flagged by <see cref="OnnxGpuAdapter.IsSoftware"/>).</summary>
    /// <returns>The adapters, or an empty list off Windows or when DXGI is unavailable.
    /// Never throws.</returns>
    public static IReadOnlyList<OnnxGpuAdapter> Enumerate()
    {
        if (!OperatingSystem.IsWindows()) return [];
        try
        {
            return EnumerateCore();
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or COMException or SEHException)
        {
            return [];
        }
    }

    private static unsafe List<OnnxGpuAdapter> EnumerateCore()
    {
        var result = new List<OnnxGpuAdapter>();
        var iid = IidDxgiFactory1;
        if (CreateDXGIFactory1(&iid, out var factory) < 0 || factory == IntPtr.Zero) return result;
        try
        {
            var enumAdapters1 = (delegate* unmanaged[Stdcall]<IntPtr, uint, out IntPtr, int>)VtableSlot(factory, SlotEnumAdapters1);
            for (uint i = 0; i < 64; i++)
            {
                var hr = enumAdapters1(factory, i, out var adapter);
                if (hr == DxgiErrorNotFound || hr < 0 || adapter == IntPtr.Zero) break;
                try
                {
                    var getDesc1 = (delegate* unmanaged[Stdcall]<IntPtr, DxgiAdapterDesc1*, int>)VtableSlot(adapter, SlotGetDesc1);
                    DxgiAdapterDesc1 desc;
                    if (getDesc1(adapter, &desc) < 0) continue;
                    result.Add(new OnnxGpuAdapter(
                        (int)i,
                        new string(desc.Description).TrimEnd('\0').Trim(),
                        desc.VendorId,
                        (ulong)desc.DedicatedVideoMemory,
                        (ulong)desc.SharedSystemMemory,
                        (desc.Flags & DxgiAdapterFlagSoftware) != 0));
                }
                finally
                {
                    Release(adapter);
                }
            }
        }
        finally
        {
            Release(factory);
        }

        return result;
    }

    private static unsafe IntPtr VtableSlot(IntPtr comObject, int slot) => (*(IntPtr**)comObject)[slot];

    private static unsafe void Release(IntPtr comObject)
    {
        var release = (delegate* unmanaged[Stdcall]<IntPtr, uint>)VtableSlot(comObject, SlotRelease);
        release(comObject);
    }

    [DllImport("dxgi.dll", ExactSpelling = true)]
    private static extern unsafe int CreateDXGIFactory1(Guid* riid, out IntPtr factory);

    [StructLayout(LayoutKind.Sequential)]
    private unsafe struct DxgiAdapterDesc1
    {
        public fixed char Description[128];
        public uint VendorId;
        public uint DeviceId;
        public uint SubSysId;
        public uint Revision;
        public nuint DedicatedVideoMemory;
        public nuint DedicatedSystemMemory;
        public nuint SharedSystemMemory;
        public uint AdapterLuidLow;
        public int AdapterLuidHigh;
        public uint Flags;
    }
}
