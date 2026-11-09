using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// What this PC has (GPU and its memory, CPU, RAM, the main display) and the settings that
    /// suit it: upscaler, quality preset, output resolution, frame rate cap and motion vectors.
    /// The GPU comes from the display adapters' registry keys (the one with the most memory, so
    /// the discrete card on a laptop), the display from its current mode.
    /// </summary>
    public sealed record Hardware(string Gpu, string Vendor, long VramBytes, string Cpu, int Threads, long RamBytes,
        int ScreenWidth, int ScreenHeight, int RefreshHz)
    {
        public enum Tier { Weak, Low, Mid, High }

        /// <summary>One setting the recommendation would change.</summary>
        public sealed record Change(string Label, string From, string To, Action Apply);

        public int VramGb => (int)Math.Round(VramBytes / 1073741824.0);
        public bool Rtx => Vendor == "NVIDIA" && Gpu.Contains("RTX", StringComparison.OrdinalIgnoreCase);
        /// <summary>Radeon RX 9000 (RDNA 4): FSR 4 runs natively.</summary>
        public bool Rdna4 => Vendor == "AMD" && System.Text.RegularExpressions.Regex.IsMatch(Gpu, @"RX\s*9\d{3}");
        public bool Integrated =>
            (VramBytes > 0 && VramBytes < 1L << 30) ||
            Gpu.Contains("UHD", StringComparison.OrdinalIgnoreCase) ||
            Gpu.Contains("Iris", StringComparison.OrdinalIgnoreCase) ||
            Gpu.Contains("Custom GPU", StringComparison.OrdinalIgnoreCase) ||
            (Vendor == "Intel" && !Gpu.Contains("Arc", StringComparison.OrdinalIgnoreCase)) ||
            (Vendor == "AMD" && !Has(@"\bRX\b|\bR9\b|\bR7\b|\bPro\b|\bVega (56|64)\b|FirePro") &&
             Gpu.Contains("Graphics", StringComparison.OrdinalIgnoreCase));

        private bool Has(string pattern) =>
            System.Text.RegularExpressions.Regex.IsMatch(Gpu, pattern, System.Text.RegularExpressions.RegexOptions.IgnoreCase);

        private int? Number(string pattern) =>
            System.Text.RegularExpressions.Regex.Match(Gpu, pattern, System.Text.RegularExpressions.RegexOptions.IgnoreCase) is { Success: true } m
                ? int.Parse(m.Groups[1].Value) : null;

        public Tier VramTier => VramGb >= 12 ? Tier.High : VramGb >= 8 ? Tier.Mid : VramGb >= 4 ? Tier.Low : Tier.Weak;

        public Tier? ModelTier
        {
            get
            {
                Tier? tier = null;
                if (Vendor == "NVIDIA")
                {
                    if (Number(@"RTX\s*(\d{4})") is int rtx)
                    {
                        int gen = rtx / 100, model = rtx % 100;
                        tier = gen >= 40 ? model >= 70 ? Tier.High : model >= 60 ? Tier.Mid : Tier.Low
                             : gen == 30 ? model >= 80 ? Tier.High : model >= 60 ? Tier.Mid : Tier.Low
                             : model >= 70 ? Tier.Mid : Tier.Low;
                    }
                    else if (Number(@"GTX\s*(\d{3,4})") is int gtx)
                        tier = gtx >= 1600 ? Tier.Low : gtx == 1080 && Has(@"\bTi\b") ? Tier.Mid
                             : gtx >= 1060 && gtx < 1100 ? Tier.Low : Tier.Weak;
                    else if (Has(@"\bGT\s*\d|\bMX\s*\d|Quadro K|GeForce \d{3}"))
                        tier = Tier.Weak;
                }
                else if (Vendor == "AMD")
                {
                    if (Number(@"RX\s*(\d{4})") is int rx)
                    {
                        int series = rx / 1000, model = rx / 10 % 100;
                        tier = series >= 9 ? rx / 10 % 10 >= 7 ? Tier.High : Tier.Mid
                             : series >= 6 ? model >= 80 ? Tier.High : model >= 60 ? Tier.Mid : Tier.Low
                             : series == 5 ? model >= 70 ? Tier.Mid : model >= 60 ? Tier.Low : Tier.Weak
                             : Tier.Low;
                    }
                    else if (Number(@"RX\s*(\d{3})\b") is int polaris)
                        tier = polaris >= 470 && VramGb >= 8 ? Tier.Low : Tier.Weak;
                    else if (Has(@"Vega (56|64)|Radeon VII"))
                        tier = Tier.Low;
                    else if (Has(@"\bR9\b|\bR7\b|\bHD\s*\d"))
                        tier = Tier.Weak;
                }
                else if (Vendor == "Intel" && Has(@"\bArc\b"))
                    tier = Has(@"\b(A7\d\d|B5\d\d|B7\d\d)\b") ? Tier.Mid : Has(@"\bA5\d\d\b") ? Tier.Low : Tier.Weak;

                if (tier is { } t && t > Tier.Weak && Has(@"Laptop|Mobile|Max-Q|\b\d{4}M\b"))
                    tier = t - 1;
                return tier;
            }
        }

        public Tier GpuTier
        {
            get
            {
                if (Integrated) return Tier.Weak;
                var memory = VramBytes > 0 ? VramTier : Tier.Low;
                return ModelTier is { } model ? (Tier)Math.Min((int)model, (int)memory) : memory;
            }
        }

        public bool RayTracingCapable =>
            Rtx || (Vendor == "AMD" && Number(@"RX\s*(\d{4})") is >= 6000) || (Vendor == "Intel" && Has(@"\bArc\b"));

        /// <summary>One line for the UI: "NVIDIA GeForce RTX 5060 (8 GB) · AMD Ryzen 5 5600 (12 threads) · 2560 × 1440 at 165 Hz".</summary>
        public string Summary =>
            L.F($"{Gpu}{(VramBytes > 0 ? $" ({VramGb} GB)" : "")} · {Cpu} ({Threads} threads) · ") +
            (ScreenWidth > 0 ? L.F($"{ScreenWidth} × {ScreenHeight} at {RefreshHz} Hz") : L.T("display unknown"));

        private static Hardware? _detected;
        public static Hardware Detect() => _detected ??= Probe();

        private static Hardware Probe()
        {
#if AVALONIA
            return OperatingSystem.IsWindows() ? ProbeWindows() : ProbeLinux();
        }

        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
        private static Hardware ProbeWindows()
        {
#endif
            var (gpu, vendor, vram) = DetectGpu();
            var cpu = "CPU";
            try
            {
                using var key = Registry.LocalMachine.OpenSubKey(@"HARDWARE\DESCRIPTION\System\CentralProcessor\0");
                if (key?.GetValue("ProcessorNameString") is string name && name.Trim().Length > 0) cpu = name.Trim();
            }
            catch { /* unreadable: "CPU" */ }
            var ram = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes;
            var mode = new DevMode { dmSize = (short)Marshal.SizeOf<DevMode>() };
            var (w, h, hz) = EnumDisplaySettings(null, EnumCurrentSettings, ref mode)
                ? (mode.dmPelsWidth, mode.dmPelsHeight, mode.dmDisplayFrequency)
                : (0, 0, 60);
            return new Hardware(gpu, vendor, vram, cpu, Environment.ProcessorCount, ram, w, h, Math.Max(hz, 1));
        }

#if AVALONIA
        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
#endif
        private static (string Name, string Vendor, long Vram) DetectGpu()
        {
            if (DetectGpuDxgi() is { } present) return present;
            var best = ("GPU", "", 0L);
            try
            {
                using var adapters = Registry.LocalMachine.OpenSubKey(
                    @"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}");
                foreach (var sub in adapters?.GetSubKeyNames() ?? Array.Empty<string>())
                {
                    if (sub.Length != 4 || !sub.All(char.IsDigit)) continue;
                    using var key = adapters!.OpenSubKey(sub);
                    if (key?.GetValue("DriverDesc") is not string name || name.Contains("Basic Display", StringComparison.OrdinalIgnoreCase)) continue;
                    var id = (key.GetValue("MatchingDeviceId") as string ?? "").ToUpperInvariant();
                    var vendor = id.Contains("VEN_10DE") ? "NVIDIA" : id.Contains("VEN_1002") ? "AMD" : id.Contains("VEN_8086") ? "Intel" : "";
                    long memory = key.GetValue("HardwareInformation.qwMemorySize") switch
                    {
                        long q => q,
                        byte[] b when b.Length >= 8 => BitConverter.ToInt64(b, 0),
                        _ => key.GetValue("HardwareInformation.MemorySize") switch
                        {
                            int d => (uint)d,
                            byte[] b when b.Length >= 4 => BitConverter.ToUInt32(b, 0),
                            _ => 0L,
                        },
                    };
                    if (memory > best.Item3 || best.Item1 == "GPU") best = (name.Trim(), vendor, memory);
                }
            }
            catch { /* no access: unknown GPU */ }
            return best;
        }

        [StructLayout(LayoutKind.Sequential)]
        private unsafe struct AdapterDesc1
        {
            public fixed char Description[128];
            public uint VendorId, DeviceId, SubSysId, Revision;
            public UIntPtr DedicatedVideoMemory, DedicatedSystemMemory, SharedSystemMemory;
            public uint LuidLow;
            public int LuidHigh;
            public uint Flags;
        }

        [DllImport("dxgi.dll")]
        private static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr factory);

        private static IntPtr Method(IntPtr com, int slot) => Marshal.ReadIntPtr(Marshal.ReadIntPtr(com), slot * IntPtr.Size);

        private static unsafe (string Name, string Vendor, long Vram)? DetectGpuDxgi()
        {
            var factory = IntPtr.Zero;
            try
            {
                var iid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");
                if (CreateDXGIFactory1(ref iid, out factory) < 0 || factory == IntPtr.Zero) return null;
                var enumAdapters = (delegate* unmanaged[Stdcall]<IntPtr, uint, IntPtr*, int>)Method(factory, 12);
                (string, string, long)? best = null;
                IntPtr adapter;
                for (uint i = 0; enumAdapters(factory, i, &adapter) >= 0 && adapter != IntPtr.Zero; i++)
                {
                    try
                    {
                        AdapterDesc1 desc;
                        var getDesc = (delegate* unmanaged[Stdcall]<IntPtr, AdapterDesc1*, int>)Method(adapter, 10);
                        if (getDesc(adapter, &desc) < 0 || (desc.Flags & 2) != 0) continue;
                        if (desc.VendorId == 0x1414) continue;
                        var vendor = desc.VendorId switch { 0x10DE => "NVIDIA", 0x1002 => "AMD", 0x8086 => "Intel", _ => "" };
                        var memory = (long)desc.DedicatedVideoMemory.ToUInt64();
                        if (best == null || memory > best.Value.Item3) best = (new string(desc.Description).Trim(), vendor, memory);
                    }
                    finally { Marshal.Release(adapter); }
                }
                return best;
            }
            catch { return null; }
            finally { if (factory != IntPtr.Zero) Marshal.Release(factory); }
        }

#if AVALONIA
        // ---------------------------------------------------------------- Linux

        /// <summary>Linux: the CPU from /proc/cpuinfo, the GPUs from /sys/class/drm (named by lspci,
        /// memory from amdgpu's sysfs file or nvidia-smi), the display from the launcher's screen.</summary>
        private static Hardware ProbeLinux()
        {
            var cpu = "CPU";
            try
            {
                var line = File.ReadLines("/proc/cpuinfo").FirstOrDefault(l => l.StartsWith("model name", StringComparison.Ordinal));
                if (line?.Split(':', 2) is [_, var name] && name.Trim().Length > 0) cpu = name.Trim();
            }
            catch { /* unreadable: "CPU" */ }
            var (gpu, vendor, vram) = DetectGpuLinux();
            var ram = GC.GetGCMemoryInfo().TotalAvailableMemoryBytes;
            var (w, h, hz) = DisplayLinux();
            return new Hardware(gpu, vendor, vram, cpu, Environment.ProcessorCount, ram, w, h, Math.Max(hz, 1));
        }

        private static (string Name, string Vendor, long Vram) DetectGpuLinux()
        {
            var best = ("GPU", "", 0L);
            try
            {
                foreach (var card in Directory.GetDirectories("/sys/class/drm", "card*"))
                {
                    if (Path.GetFileName(card).Contains('-')) continue; // a connector (card0-DP-1), not a GPU
                    var device = Path.Combine(card, "device");
                    var id = ReadText(Path.Combine(device, "vendor")).ToLowerInvariant();
                    if (id.Length == 0) continue;
                    var vendor = id == "0x10de" ? "NVIDIA" : id == "0x1002" ? "AMD" : id == "0x8086" ? "Intel" : "";
                    // The PCI slot (0000:01:00.0) names the card for lspci.
                    var slot = new DirectoryInfo(device).LinkTarget is { } target ? Path.GetFileName(target) : "";
                    var label = PciName(slot) ?? (vendor.Length > 0 ? vendor + " GPU" : "GPU");
                    long memory = long.TryParse(ReadText(Path.Combine(device, "mem_info_vram_total")), out var amd) ? amd : 0;
                    if (memory == 0 && vendor == "NVIDIA") memory = NvidiaMemory();
                    if (memory > best.Item3 || best.Item1 == "GPU") best = (label, vendor, memory);
                }
            }
            catch { /* no sysfs: unknown GPU */ }
            return best;
        }

        private static string ReadText(string path)
        {
            try { return File.Exists(path) ? File.ReadAllText(path).Trim() : ""; }
            catch { return ""; }
        }

        /// <summary>"NVIDIA GeForce RTX 4070" from lspci -mm's vendor and device fields; null without lspci.</summary>
        private static string? PciName(string slot)
        {
            if (slot.Length == 0 || Run("lspci", $"-mm -s {slot}") is not { Length: > 0 } output) return null;
            var fields = System.Text.RegularExpressions.Regex.Matches(output, "\"([^\"]*)\"").Select(m => m.Groups[1].Value).ToList();
            if (fields.Count < 3) return null;
            var vendor = fields[1].Replace(" Corporation", "").Replace("Advanced Micro Devices, Inc. [AMD/ATI]", "AMD");
            // "AD104 [GeForce RTX 4070]": the marketing name in brackets.
            var device = System.Text.RegularExpressions.Regex.Match(fields[2], @"\[([^\]]+)\]") is { Success: true } m ? m.Groups[1].Value : fields[2];
            return $"{vendor} {device}".Trim();
        }

        private static long NvidiaMemory() =>
            Run("nvidia-smi", "--query-gpu=memory.total --format=csv,noheader,nounits")?.Split('\n')
                .Select(l => long.TryParse(l.Trim(), out var mib) ? mib << 20 : 0).DefaultIfEmpty(0).Max() ?? 0;

        /// <summary>The primary screen's size from the launcher's window, its refresh rate from xrandr.</summary>
        private static (int W, int H, int Hz) DisplayLinux()
        {
            var screen = (Avalonia.Application.Current?.ApplicationLifetime as Avalonia.Controls.ApplicationLifetimes.IClassicDesktopStyleApplicationLifetime)
                ?.MainWindow?.Screens.Primary;
            var (w, h) = screen == null ? (0, 0) : (screen.Bounds.Width, screen.Bounds.Height);
            var hz = 60;
            if (Run("xrandr", "--current") is { } modes &&
                System.Text.RegularExpressions.Regex.Match(modes, @"(\d+)x(\d+)\s+[^\n]*?(\d+(?:\.\d+)?)\*") is { Success: true } current)
            {
                if (w == 0) (w, h) = (int.Parse(current.Groups[1].Value), int.Parse(current.Groups[2].Value));
                hz = (int)Math.Round(double.Parse(current.Groups[3].Value, System.Globalization.CultureInfo.InvariantCulture));
            }
            return (w, h, hz);
        }

        /// <summary>A tool's output, or null when it is not installed or fails.</summary>
        private static string? Run(string tool, string args)
        {
            try
            {
                using var process = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(tool, args)
                    { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false });
                if (process == null) return null;
                var output = process.StandardOutput.ReadToEnd();
                return process.WaitForExit(3000) && process.ExitCode == 0 ? output : null;
            }
            catch
            {
                return null;
            }
        }

#endif
        // ---------------------------------------------------------------- recommendation

        /// <summary>How the player wants the game to run (the setup guide's last choice): each style is
        /// still sized to this PC. Balanced is what Recommend picks without a style.</summary>
        public const string StyleQuality = "quality", StyleBalanced = "balanced", StyleSmooth = "smooth", StyleFps = "fps";

        public static readonly Choice[] Styles =
        {
            new(L.T("Best picture"), StyleQuality,
                L.T("The sharpest image: the game renders at or close to your screen's resolution, with the extra detail on, at 60 FPS. Best on strong graphics cards; a weaker one may drop frames in busy fights.")),
            new(L.T("Balanced (recommended)"), StyleBalanced,
                L.T("A good picture at a steady frame rate, picked for your graphics card and screen. The best start if you are not sure.")),
            new(L.T("Smoother"), StyleSmooth,
                L.T("A higher frame rate (90 or 120 FPS when your screen and processor can show it) for a slightly softer picture: the game renders further below your screen's resolution and the upscaler fills in the rest.")),
            new(L.T("Maximum FPS"), StyleFps,
                L.T("As many frames as possible: the game renders well below your screen's resolution, with less model detail and the extras off, up to what your screen can show. For weaker PCs or fast screens; the picture is softer.")),
        };

        /// <summary>What would change to suit this PC; empty when the settings already do. With a
        /// style (Styles), the picture and frame rate lean that way and model detail is set too.</summary>
        public List<Change> Recommend(LauncherSettings s, IniFile ini, string? style = null)
        {
            var changes = new List<Change>();
            void Ini(string label, string key, string to, IReadOnlyList<Choice>? names = null, string fallback = "")
            {
                var from = ini.Get(key, fallback);
                if (from == to) return;
                changes.Add(new Change(label, Name(names, from), Name(names, to), () => ini.Set(key, to)));
            }
            void Setting(string label, string key, string to, IReadOnlyList<Choice>? names = null)
            {
                var from = s.Str(key);
                if (from == to) return;
                changes.Add(new Change(label, Name(names, from), Name(names, to), () => s.Set(key, to)));
            }

            var tier = GpuTier;
            var dlss = Paths.DlssRuntime != null;
            Ini(L.T("Upscaler"), "upscaler", Rtx && dlss ? "dlss" : OperatingSystem.IsWindows() ? "fsr" : Rdna4 ? "fsr4" : "fsr3", Choices.Upscalers);

            // The output: the display's own size, by name where it is one of the choices, else
            // "Display (auto)"; 1920 x 1080 when the display is unknown.
            var output = SteamDeck.Detected ? "1280x720"
                : ScreenWidth == 0 ? "1920x1080"
                : tier == Tier.Weak && ScreenHeight > 1080 ? "1920x1080"
                : Choices.OutputResolutions.Select(c => c.Value).FirstOrDefault(v => v == $"{ScreenWidth}x{ScreenHeight}") ?? "auto";
            Ini(L.T("Output resolution"), "output_res", output, Choices.OutputResolutions, "1920x1080");

            // How far below the output the game renders: more for bigger outputs and smaller GPUs.
            var height = Choices.OutputSize(output).H;
            var preset = (height, tier) switch
            {
                (<= 720, Tier.Weak) => 2,
                (<= 1080, Tier.Weak) => 3,
                (_, Tier.Weak) => 4,
                (<= 720, Tier.Low) => 1,
                (<= 720, _) => 0,
                (<= 1080, Tier.High) => 0,
                (<= 1080, Tier.Mid) => 1,
                (<= 1080, _) => 2,
                (<= 1440, Tier.High) => 1,
                (<= 1440, Tier.Mid) => 2,
                (<= 1440, _) => 3,
                (_, Tier.High) => 2,
                (_, Tier.Mid) => 3,
                _ => 4,
            };
            preset = style switch
            {
                StyleQuality => Math.Max(0, preset - 1),
                StyleSmooth => Math.Min(4, preset + 1),
                StyleFps => Math.Min(4, Math.Max(3, preset + 2)),
                _ => preset,
            };
            Ini(L.T("Quality preset"), "preset", preset.ToString(), Choices.Presets, "0");

            // 120 needs a fast display, a mid or better GPU and a CPU with 12+ threads (the game's
            // render thread is the limit); 60 otherwise.
            var fps = tier == Tier.Weak || Threads < 6 ? "30"
                : RefreshHz >= 119 && tier >= Tier.Mid && Threads >= 12 ? "120" : "60";
            // Best picture stays at 60; Smoother takes 90 where 120 is out of reach; Maximum FPS goes
            // up to the display's own rate (the render thread may hold it lower on small CPUs).
            fps = style switch
            {
                StyleQuality => fps == "30" ? "30" : "60",
                StyleSmooth => fps == "120" ? "120" : tier == Tier.Weak ? "60" : RefreshHz >= 89 && Threads >= 8 ? "90" : "60",
                StyleFps => tier == Tier.Weak ? "60"
                    : RefreshHz >= 143 && tier >= Tier.Mid ? "144" : RefreshHz >= 119 ? "120" : RefreshHz >= 89 ? "90" : "60",
                _ => fps,
            };
            Setting(L.T("Frame rate"), "fps_mode", fps, Choices.FpsModes);

            // Character motion vectors cost about 10%: on for the biggest GPUs, off for the smallest.
            if (style == StyleQuality) Ini(L.T("Object motion vectors"), "object_motion", "1", OnOff, "1");
            else if (style == StyleFps) Ini(L.T("Object motion vectors"), "object_motion", "0", OnOff, "1");
            else if (tier == Tier.High) Ini(L.T("Object motion vectors"), "object_motion", "1", OnOff, "1");
            else if (tier <= Tier.Low || style == StyleSmooth) Ini(L.T("Object motion vectors"), "object_motion", "0", OnOff, "1");

            if (style != null || tier == Tier.Weak)
                Ini(L.T("Model detail"), "model_lod",
                    style == StyleQuality && tier == Tier.High ? "-2"
                    : tier == Tier.Weak ? style == StyleFps ? "2" : "1"
                    : style == StyleFps ? "1" : "0",
                    Choices.ModelLod, "0");

            var heavy = new[] { "effect_gi", "effect_volumetric_light", "effect_point_fog", "effect_point_shadows",
                                "effect_pcss", "effect_contact_shadows", "effect_gtao", "effect_modern_ssr", "effect_ssr" };
            foreach (var effect in Choices.Effects)
            {
                var off = (tier <= Tier.Low || style == StyleFps) && heavy.Contains(effect.Key)
                       || effect.Key == "effect_ray_tracing" && (!RayTracingCapable || tier < Tier.High)
                       || tier == Tier.Weak && effect.Key is "effect_ssao" or "effect_dynamic_shadows";
                if (off) Ini(effect.Title, effect.Key, "0", OnOff, effect.Default ? "1" : "0");
            }
            return changes;
        }

        private static readonly Choice[] OnOff = { new(L.T("On"), "1"), new(L.T("Off"), "0") };

        private static string Name(IReadOnlyList<Choice>? names, string value) =>
            names?.FirstOrDefault(c => c.Value == value)?.Label.Split(" (")[0] ?? (value.Length > 0 ? value : L.T("default"));

        // ---------------------------------------------------------------- display mode

        private const int EnumCurrentSettings = -1;

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern bool EnumDisplaySettings(string? deviceName, int modeNum, ref DevMode devMode);

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct DevMode
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
            public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra;
            public int dmFields, dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
            public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
            public short dmLogPixels;
            public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
            public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
        }
    }
}
