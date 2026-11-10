using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
#if AVALONIA
using Avalonia.Input;
#else
using System.Windows.Input;
#endif

namespace BloodborneLauncher.Core
{
    public sealed record ControlDef(string Name, string Label, string Group, string Hint, string Default, string? Pad);

    public static class InputMap
    {
        public static readonly ControlDef[] Controls =
        {
            new("move_forward", L.T("Move forward"), L.T("Movement"), "", "W", "Left stick"),
            new("move_back", L.T("Move back"), L.T("Movement"), "", "S", "Left stick"),
            new("move_left", L.T("Move left"), L.T("Movement"), "", "A", "Left stick"),
            new("move_right", L.T("Move right"), L.T("Movement"), "", "D", "Left stick"),
            new("walk", L.T("Walk"), L.T("Movement"), L.T("Hold with the movement keys"), "Left Alt", null),
            new("roll", L.T("Dodge / sprint"), L.T("Movement"), L.T("Tap to dodge, hold to sprint"), "Space", "Circle"),
            new("dodge", L.T("Dodge"), L.T("Movement"), L.T("Dodges even while the sprint key is held"), "", "Circle"),
            new("backstep", L.T("Backstep"), L.T("Movement"), L.T("Steps back at once, also while moving or sprinting"), "", "Circle"),
            new("sprint", L.T("Sprint"), L.T("Movement"), L.T("Sprints at once while held, and never dodges"), "", "Circle"),
            new("l3", L.T("Jump / left stick click"), L.T("Movement"), L.T("Jumps when Jump on its own key is on"), "Left Ctrl", "L3"),
            new("attack", L.T("Attack"), L.T("Combat"), L.T("With the strong modifier held: strong attack"), "Mouse1", "R1"),
            new("strong_attack", L.T("Strong attack"), L.T("Combat"), L.T("Hold to charge"), "V", "R2"),
            new("strong_modifier", L.T("Strong attack modifier"), L.T("Combat"), L.T("Held with Attack"), "Left Shift", null),
            new("transform", L.T("Transform weapon"), L.T("Combat"), "", "C, Mouse2", "L1"),
            new("firearm", L.T("Firearm / parry"), L.T("Combat"), "", "X, Mouse3", "L2"),
            new("lock_on", L.T("Lock on"), L.T("Combat"), L.T("Or reset the camera"), "Q", "R3"),
            new("blood_vial", L.T("Blood Vial"), L.T("Items"), "", "R", "Triangle"),
            new("use_item", L.T("Use item"), L.T("Items"), L.T("The selected quick item"), "F", "Square"),
            new("interact", L.T("Interact"), L.T("Items"), L.T("Talk, examine, pick up, open"), "E", "Cross"),
            new("switch_item", L.T("Switch item"), L.T("Items"), L.T("Cycle the quick items"), "Down", "D-pad down"),
            new("switch_right", L.T("Switch right weapon"), L.T("Items"), "", "Right", "D-pad right"),
            new("switch_left", L.T("Switch left weapon"), L.T("Items"), "", "Left", "D-pad left"),
            new("dpad_up", L.T("D-pad up"), L.T("Items"), "", "Up", "D-pad up"),
            new("gestures", L.T("Gestures"), L.T("Menus"), "", "G", "Touchpad (left)"),
            new("personal_effects", L.T("Personal effects"), L.T("Menus"), "", "T", "Touchpad (right)"),
            new("menu", L.T("Menu"), L.T("Menus"), "", "Escape", "Options"),
            new("confirm", L.T("Confirm in menus"), L.T("Menus"), "", "Return", "Menu confirm"),
            new("back", L.T("Back in menus"), L.T("Menus"), "", "Backspace", "Menu back"),
            new("look_up", L.T("Look up"), L.T("Camera keys"), L.T("The camera without the mouse"), "I", "Right stick"),
            new("look_down", L.T("Look down"), L.T("Camera keys"), "", "K", "Right stick"),
            new("look_left", L.T("Look left"), L.T("Camera keys"), "", "J", "Right stick"),
            new("look_right", L.T("Look right"), L.T("Camera keys"), "", "L", "Right stick"),
        };

        public static readonly Dictionary<string, string> Reserved = new(StringComparer.OrdinalIgnoreCase)
        {
            ["Insert"] = "Insert opens the port's settings menu",
        };

        public static readonly string[] MouseTokens = { "Mouse1", "Mouse2", "Mouse3", "Mouse4", "Mouse5", "WheelUp", "WheelDown" };

        public static bool IsMouse(string name) => MouseTokens.Contains(Base(name), StringComparer.OrdinalIgnoreCase);

        private static readonly string[] ModifierPrefixes = { "Ctrl+", "Shift+", "Alt+" };

        public static string Base(string name)
        {
            for (var stripped = true; stripped;)
            {
                stripped = false;
                foreach (var prefix in ModifierPrefixes)
                    if (name.Length > prefix.Length && name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                    {
                        name = name[prefix.Length..].Trim();
                        stripped = true;
                    }
            }
            return name;
        }

        public static string WithModifiers(string name, bool ctrl, bool shift, bool alt)
        {
            if (IsModifierName(name)) return name;
            return (ctrl ? "Ctrl+" : "") + (shift ? "Shift+" : "") + (alt ? "Alt+" : "") + name;
        }

        public static bool IsModifierName(string name) =>
            name is "Left Shift" or "Right Shift" or "Left Ctrl" or "Right Ctrl" or "Left Alt" or "Right Alt";

        public static bool IsModifierKey(Key key) =>
            key is Key.LeftShift or Key.RightShift or Key.LeftCtrl or Key.RightCtrl or Key.LeftAlt or Key.RightAlt;

        public static string Display(string name)
        {
            var input = Base(name);
            return name[..^input.Length].Replace("+", " + ") + DisplayInput(input);
        }

        private static string DisplayInput(string name) => name.ToLowerInvariant() switch
        {
            "mouse1" => L.T("Left click"),
            "mouse2" => L.T("Middle click"),
            "mouse3" => L.T("Right click"),
            "mouse4" => L.T("Mouse 4"),
            "mouse5" => L.T("Mouse 5"),
            "wheelup" => L.T("Wheel up"),
            "wheeldown" => L.T("Wheel down"),
            _ => name,
        };

        public static List<string> Split(string value) =>
            value.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .Where(n => !n.Equals("none", StringComparison.OrdinalIgnoreCase)).ToList();

        public static string Join(IEnumerable<string> names) => string.Join(", ", names);

        public static string? SdlKeyName(Key key)
        {
            if (key >= Key.A && key <= Key.Z) return key.ToString();
            if (key >= Key.D0 && key <= Key.D9) return ((char)('0' + (key - Key.D0))).ToString();
            if (key >= Key.NumPad0 && key <= Key.NumPad9) return "Keypad " + (char)('0' + (key - Key.NumPad0));
            if (key >= Key.F1 && key <= Key.F24) return "F" + (key - Key.F1 + 1);

            return key switch
            {
                Key.Space => "Space",
                Key.Return => "Return",
                Key.Escape => "Escape",
                Key.Back => "Backspace",
                Key.Tab => "Tab",
                Key.CapsLock => "CapsLock",
                Key.LeftShift => "Left Shift",
                Key.RightShift => "Right Shift",
                Key.LeftCtrl => "Left Ctrl",
                Key.RightCtrl => "Right Ctrl",
                Key.LeftAlt => "Left Alt",
                Key.RightAlt => "Right Alt",
                Key.LWin => "Left GUI",
                Key.RWin => "Right GUI",
                Key.Apps => "Application",
                Key.Up => "Up",
                Key.Down => "Down",
                Key.Left => "Left",
                Key.Right => "Right",
                Key.Insert => "Insert",
                Key.Delete => "Delete",
                Key.Home => "Home",
                Key.End => "End",
                Key.PageUp => "PageUp",
                Key.PageDown => "PageDown",
                Key.PrintScreen => "PrintScreen",
                Key.Scroll => "ScrollLock",
                Key.Pause => "Pause",
                Key.NumLock => "Numlock",
                Key.Divide => "Keypad /",
                Key.Multiply => "Keypad *",
                Key.Subtract => "Keypad -",
                Key.Add => "Keypad +",
                Key.Decimal => "Keypad .",
                Key.OemMinus => "-",
                Key.OemPlus => "=",
                Key.OemOpenBrackets => "[",
                Key.OemCloseBrackets => "]",
                Key.OemPipe => "\\",
                Key.OemSemicolon => ";",
                Key.OemQuotes => "'",
                Key.OemTilde => "`",
                Key.OemComma => ",",
                Key.OemPeriod => ".",
                Key.OemQuestion => "/",
                Key.OemBackslash => "NonUSBackslash",
                _ => null,
            };
        }

        public static string? MouseToken(MouseButton button) => button switch
        {
            MouseButton.Left => "Mouse1",
            MouseButton.Middle => "Mouse2",
            MouseButton.Right => "Mouse3",
            MouseButton.XButton1 => "Mouse4",
            MouseButton.XButton2 => "Mouse5",
            _ => null,
        };

        private static readonly Lazy<HashSet<string>> KeyNames = new(() =>
        {
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var key in Enum.GetValues<Key>())
                if (SdlKeyName(key) is { } name) names.Add(name);
            return names;
        });

        public static string? Canonical(string name)
        {
            name = name.Trim();
            var input = Base(name);
            var mods = name[..^input.Length];
            var ctrl = mods.Contains("ctrl", StringComparison.OrdinalIgnoreCase);
            var shift = mods.Contains("shift", StringComparison.OrdinalIgnoreCase);
            var alt = mods.Contains("alt", StringComparison.OrdinalIgnoreCase);
            if (MouseTokens.FirstOrDefault(t => t.Equals(input, StringComparison.OrdinalIgnoreCase)) is { } mouse)
                return WithModifiers(mouse, ctrl, shift, alt);
            return KeyNames.Value.FirstOrDefault(k => k.Equals(input, StringComparison.OrdinalIgnoreCase)) is { } key
                ? WithModifiers(key, ctrl, shift, alt)
                : null;
        }
    }

    public static class Bindings
    {
        public static string IniKey(ControlDef c) => "bind." + c.Name;

        public static List<string> Get(IniFile ini, ControlDef c) => InputMap.Split(ini.Raw(IniKey(c)) ?? c.Default);

        public static bool IsDefault(IniFile ini, ControlDef c) =>
            Same(Get(ini, c), InputMap.Split(c.Default));

        private static bool Same(List<string> a, List<string> b) =>
            a.Count == b.Count && a.Zip(b).All(p => p.First.Equals(p.Second, StringComparison.OrdinalIgnoreCase));

        public const int MaxInputs = 5;

        public static void Set(IniFile ini, ControlDef c, IEnumerable<string> names)
        {
            var list = names.Distinct(StringComparer.OrdinalIgnoreCase).Take(MaxInputs).ToList();
            var value = InputMap.Join(list);
            ini.Set(IniKey(c), value.Length == 0 ? "none" : value);
        }

        public static void Assign(IniFile ini, ControlDef c, string name, bool keepOtherKind)
        {
            var current = keepOtherKind ? Get(ini, c).Where(n => InputMap.IsMouse(n) != InputMap.IsMouse(name)) : Enumerable.Empty<string>();
            Set(ini, c, current.Append(name));
        }

        public static void Add(IniFile ini, ControlDef c, string name)
        {
            var current = Get(ini, c);
            if (current.Contains(name, StringComparer.OrdinalIgnoreCase)) return;
            if (current.Count >= MaxInputs) current.RemoveAt(current.Count - 1);
            current.Add(name);
            Set(ini, c, current);
        }

        public static void ResetAll(IniFile ini)
        {
            foreach (var c in InputMap.Controls)
                ini.Set(IniKey(c), c.Default);
        }

        public static Dictionary<string, string> Current(IniFile ini) =>
            InputMap.Controls.ToDictionary(c => c.Name, c => InputMap.Join(Get(ini, c)));
    }

    public static class Gamepads
    {
        public sealed record Pad(string Name, bool Bluetooth);

        private static readonly Dictionary<(int Vid, int Pid), string> Known = new()
        {
            [(0x054C, 0x05C4)] = "DualShock 4",
            [(0x054C, 0x09CC)] = "DualShock 4",
            [(0x054C, 0x0BA0)] = "DualShock 4 (wireless adapter)",
            [(0x054C, 0x0CE6)] = "DualSense",
            [(0x054C, 0x0DF2)] = "DualSense Edge",
            [(0x054C, 0x0268)] = "DualShock 3",
            [(0x057E, 0x2009)] = "Switch Pro Controller",
            [(0x20BC, 0x5500)] = "Betop BFM",
        };

        private static readonly Regex VidPid = new(@"vid[_&](?:0002)?([0-9a-f]{4}).*?pid[_&]([0-9a-f]{4})",
            RegexOptions.IgnoreCase | RegexOptions.Compiled);

        public static List<Pad> Detect()
        {
#if AVALONIA
            if (!OperatingSystem.IsWindows()) return DetectLinux();
#endif
            var pads = new List<Pad>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var path in HidPaths())
            {
                var match = VidPid.Match(path);
                if (!match.Success) continue;
                var key = (Convert.ToInt32(match.Groups[1].Value, 16), Convert.ToInt32(match.Groups[2].Value, 16));
                if (path.Contains("ig_", StringComparison.OrdinalIgnoreCase)) continue;
                if (!Known.TryGetValue(key, out var name) && (name = GenericGamepadName(path)) == null) continue;
                var instance = path.Split('#').ElementAtOrDefault(2) ?? path;
                var id = key.Item1 == 0x054C || key.Item1 == 0x057E
                    ? instance.Split('&').Take(2).Aggregate(name, (a, b) => a + b)
                    : $"{key.Item1:x4}:{key.Item2:x4}";
                if (seen.Add(id))
                    pads.Add(new Pad(name, path.Contains("{00001124-", StringComparison.OrdinalIgnoreCase)));
            }
            foreach (var _ in XInput.Connected())
                pads.Add(new Pad(L.T("Xbox controller"), false));
            return pads;
        }

#if AVALONIA
        private static List<Pad> DetectLinux()
        {
            var pads = new List<Pad>();
            try
            {
                var text = File.ReadAllText("/proc/bus/input/devices");
                foreach (var block in text.Split("\n\n", StringSplitOptions.RemoveEmptyEntries))
                {
                    if (!Regex.IsMatch(block, @"^H: Handlers=.*\bjs\d+", RegexOptions.Multiline)) continue;
                    var id = Regex.Match(block, @"^I: Bus=([0-9a-f]{4}) Vendor=([0-9a-f]{4}) Product=([0-9a-f]{4})", RegexOptions.Multiline | RegexOptions.IgnoreCase);
                    var own = Regex.Match(block, "^N: Name=\"(.*)\"", RegexOptions.Multiline).Groups[1].Value;
                    var name = id.Success && Known.TryGetValue((Convert.ToInt32(id.Groups[2].Value, 16), Convert.ToInt32(id.Groups[3].Value, 16)), out var known)
                        ? known
                        : own.Length > 0 ? own : L.T("Game controller");
                    pads.Add(new Pad(name, id.Success && id.Groups[1].Value == "0005"));
                }
            }
            catch
            {
            }
            return pads;
        }

#endif

        [StructLayout(LayoutKind.Sequential)]
        private struct HidCaps
        {
            public ushort Usage, UsagePage, InputReportByteLength, OutputReportByteLength, FeatureReportByteLength;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 17)] public ushort[] Reserved;
            public ushort NumberLinkCollectionNodes, NumberInputButtonCaps, NumberInputValueCaps, NumberInputDataIndices,
                NumberOutputButtonCaps, NumberOutputValueCaps, NumberOutputDataIndices,
                NumberFeatureButtonCaps, NumberFeatureValueCaps, NumberFeatureDataIndices;
        }

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
        [DllImport("hid.dll")]
        private static extern bool HidD_GetPreparsedData(Microsoft.Win32.SafeHandles.SafeFileHandle device, out IntPtr data);
        [DllImport("hid.dll")]
        private static extern bool HidD_FreePreparsedData(IntPtr data);
        [DllImport("hid.dll")]
        private static extern int HidP_GetCaps(IntPtr data, out HidCaps caps);
        [DllImport("hid.dll", CharSet = CharSet.Unicode)]
        private static extern bool HidD_GetProductString(Microsoft.Win32.SafeHandles.SafeFileHandle device, char[] buffer, int length);

        private static string? GenericGamepadName(string path)
        {
            try
            {
                using var device = CreateFileW(path, 0, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
                if (device.IsInvalid || !HidD_GetPreparsedData(device, out var data)) return null;
                try
                {
                    if (HidP_GetCaps(data, out var caps) != 0x00110000 || caps.UsagePage != 1 || caps.Usage is not (4 or 5))
                        return null;
                }
                finally
                {
                    HidD_FreePreparsedData(data);
                }
                var buffer = new char[127];
                var name = HidD_GetProductString(device, buffer, buffer.Length * 2) ? new string(buffer).TrimEnd('\0').Trim() : "";
                return name.Length > 0 ? name : L.T("Game controller");
            }
            catch
            {
                return null;
            }
        }


        private static Guid HidInterface = new("4D1E55B2-F16F-11CF-88CB-001111000030");
        private const int DIGCF_PRESENT = 0x2, DIGCF_DEVICEINTERFACE = 0x10;

        [StructLayout(LayoutKind.Sequential)]
        private struct InterfaceData
        {
            public int Size;
            public Guid Class;
            public int Flags;
            public IntPtr Reserved;
        }

        [DllImport("setupapi.dll", SetLastError = true)]
        private static extern IntPtr SetupDiGetClassDevsW(ref Guid guid, IntPtr enumerator, IntPtr parent, int flags);
        [DllImport("setupapi.dll", SetLastError = true)]
        private static extern bool SetupDiEnumDeviceInterfaces(IntPtr set, IntPtr info, ref Guid guid, int index, ref InterfaceData data);
        [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        private static extern bool SetupDiGetDeviceInterfaceDetailW(IntPtr set, ref InterfaceData data, IntPtr detail, int size, out int required, IntPtr info);
        [DllImport("setupapi.dll")]
        private static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);

        private static List<string> HidPaths()
        {
            var paths = new List<string>();
            var set = SetupDiGetClassDevsW(ref HidInterface, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
            if (set == IntPtr.Zero || set == new IntPtr(-1)) return paths;
            try
            {
                var data = new InterfaceData { Size = Marshal.SizeOf<InterfaceData>() };
                for (var i = 0; SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref HidInterface, i, ref data); i++)
                {
                    SetupDiGetDeviceInterfaceDetailW(set, ref data, IntPtr.Zero, 0, out var required, IntPtr.Zero);
                    if (required <= 0) continue;
                    var buffer = Marshal.AllocHGlobal(required);
                    try
                    {
                        Marshal.WriteInt32(buffer, IntPtr.Size == 8 ? 8 : 6);
                        if (SetupDiGetDeviceInterfaceDetailW(set, ref data, buffer, required, out _, IntPtr.Zero))
                            paths.Add(Marshal.PtrToStringUni(buffer + 4) ?? "");
                    }
                    finally
                    {
                        Marshal.FreeHGlobal(buffer);
                    }
                }
            }
            catch
            {
            }
            finally
            {
                SetupDiDestroyDeviceInfoList(set);
            }
            return paths;
        }
    }

    public static class XInput
    {
        [StructLayout(LayoutKind.Sequential)]
        private struct State
        {
            public uint PacketNumber;
            public ushort Buttons;
            public byte LeftTrigger, RightTrigger;
            public short ThumbLX, ThumbLY, ThumbRX, ThumbRY;
        }

        [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
        private static extern uint GetState(uint user, out State state);

        public static List<int> Connected()
        {
            var slots = new List<int>();
            try
            {
                for (uint i = 0; i < 4; i++)
                    if (GetState(i, out _) == 0)
                        slots.Add((int)i + 1);
            }
            catch
            {
            }
            return slots;
        }
    }
}
