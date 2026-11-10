using System.IO;
using System.Runtime.InteropServices;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The engine's own SDL3 (out\SDL3.dll in the package), used to see controllers exactly as the
    /// game does: the same names, the same button mapping (gamecontrollerdb.txt included) and the
    /// same button names for pad.* lines. Only used while the game is not running, so the two
    /// never hold a pad at the same time.
    /// </summary>
    public static unsafe class SdlPad
    {
        public sealed record Device(string Name, bool Supported);

        private const uint InitGamepad = 0x2000;
        private const int ButtonCount = 26, AxisLeftTrigger = 4, AxisRightTrigger = 5;

        private static bool _loaded, _failed, _initialised;
        private static delegate* unmanaged[Cdecl]<byte*, byte*, byte> _setHint;
        private static delegate* unmanaged[Cdecl]<uint, byte> _init;
        private static delegate* unmanaged[Cdecl]<uint, void> _quit;
        private static delegate* unmanaged[Cdecl]<int*, uint*> _gamepads, _joysticks;
        private static delegate* unmanaged[Cdecl]<void*, void> _free;
        private static delegate* unmanaged[Cdecl]<uint, IntPtr> _open, _nameForId, _joystickNameForId;
        private static delegate* unmanaged[Cdecl]<uint, byte> _isGamepad;
        private static delegate* unmanaged[Cdecl]<void> _update;
        private static delegate* unmanaged[Cdecl]<IntPtr, int, byte> _button;
        private static delegate* unmanaged[Cdecl]<IntPtr, int, short> _axis;
        private static delegate* unmanaged[Cdecl]<int, IntPtr> _buttonName;
        private static delegate* unmanaged[Cdecl]<IntPtr, void> _close;
        private static readonly Dictionary<uint, IntPtr> Open = new();

        /// <summary>False when the package has no SDL3.dll (the launcher falls back to its own detection).</summary>
        public static bool Available => Load();

        private static bool Load()
        {
            if (_loaded || _failed) return _loaded;
            try
            {
#if AVALONIA
                // Linux: the engine's own libSDL3 when it ships one, else the system's.
                IntPtr lib;
                if (!OperatingSystem.IsWindows())
                {
                    var own = Path.Combine(Paths.BinariesDir, "libSDL3.so.0");
                    if (!(File.Exists(own) && NativeLibrary.TryLoad(own, out lib)) &&
                        !NativeLibrary.TryLoad("libSDL3.so.0", out lib) && !NativeLibrary.TryLoad("libSDL3.so", out lib))
                    {
                        _failed = true;
                        return false;
                    }
                }
                else
                {
                    var dll = Path.Combine(Paths.BinariesDir, "SDL3.dll");
                    if (!File.Exists(dll)) { _failed = true; return false; }
                    lib = NativeLibrary.Load(dll);
                }
#else
                var dll = Path.Combine(Paths.BinariesDir, "SDL3.dll");
                if (!File.Exists(dll)) { _failed = true; return false; }
                var lib = NativeLibrary.Load(dll);
#endif
                IntPtr Get(string name) => NativeLibrary.GetExport(lib, name);
                _setHint = (delegate* unmanaged[Cdecl]<byte*, byte*, byte>)Get("SDL_SetHint");
                _init = (delegate* unmanaged[Cdecl]<uint, byte>)Get("SDL_InitSubSystem");
                _quit = (delegate* unmanaged[Cdecl]<uint, void>)Get("SDL_QuitSubSystem");
                _gamepads = (delegate* unmanaged[Cdecl]<int*, uint*>)Get("SDL_GetGamepads");
                _joysticks = (delegate* unmanaged[Cdecl]<int*, uint*>)Get("SDL_GetJoysticks");
                _free = (delegate* unmanaged[Cdecl]<void*, void>)Get("SDL_free");
                _open = (delegate* unmanaged[Cdecl]<uint, IntPtr>)Get("SDL_OpenGamepad");
                _nameForId = (delegate* unmanaged[Cdecl]<uint, IntPtr>)Get("SDL_GetGamepadNameForID");
                _joystickNameForId = (delegate* unmanaged[Cdecl]<uint, IntPtr>)Get("SDL_GetJoystickNameForID");
                _isGamepad = (delegate* unmanaged[Cdecl]<uint, byte>)Get("SDL_IsGamepad");
                _update = (delegate* unmanaged[Cdecl]<void>)Get("SDL_UpdateGamepads");
                _button = (delegate* unmanaged[Cdecl]<IntPtr, int, byte>)Get("SDL_GetGamepadButton");
                _axis = (delegate* unmanaged[Cdecl]<IntPtr, int, short>)Get("SDL_GetGamepadAxis");
                _buttonName = (delegate* unmanaged[Cdecl]<int, IntPtr>)Get("SDL_GetGamepadStringForButton");
                _close = (delegate* unmanaged[Cdecl]<IntPtr, void>)Get("SDL_CloseGamepad");
                _loaded = true;
            }
            catch
            {
                _failed = true;
            }
            return _loaded;
        }

        private static bool Start()
        {
            if (_initialised) return true;
            if (!Load()) return false;
            // The same hints the launcher gives the game (GameEnvironment).
            foreach (var (key, value) in GameEnvironment.SdlHints())
                SetHint(key, value);
            SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
            SetHint("SDL_JOYSTICK_HIDAPI_STEAM", "1");
            SetHint("SDL_JOYSTICK_HIDAPI_STEAMDECK", "1");
            SetHint("SDL_JOYSTICK_HIDAPI_STEAM_HORI", "1");
            _initialised = _init(InitGamepad) != 0;
            return _initialised;
        }

        private static void SetHint(string name, string value)
        {
            var key = System.Text.Encoding.UTF8.GetBytes(name + "\0");
            var text = System.Text.Encoding.UTF8.GetBytes(value + "\0");
            fixed (byte* k = key, t = text)
                _setHint(k, t);
        }

        /// <summary>Lets go of every pad (before the game starts, or when the page closes).</summary>
        public static void Stop()
        {
            if (!_initialised) return;
            foreach (var pad in Open.Values) _close(pad);
            Open.Clear();
            _quit(InitGamepad);
            _initialised = false;
        }

        private static List<uint> Ids(delegate* unmanaged[Cdecl]<int*, uint*> list)
        {
            var ids = new List<uint>();
            int count;
            var array = list(&count);
            if (array == null) return ids;
            for (var i = 0; i < count; i++) ids.Add(array[i]);
            _free(array);
            return ids;
        }

        /// <summary>The controllers connected now; Supported is false for one SDL sees but cannot map.</summary>
        public static List<Device>? Devices()
        {
            if (!Start()) return null;
            _update();
            var devices = new List<Device>();
            foreach (var id in Ids(_joysticks))
            {
                var supported = _isGamepad(id) != 0;
                var name = Marshal.PtrToStringUTF8(supported ? _nameForId(id) : _joystickNameForId(id));
                devices.Add(new Device(string.IsNullOrWhiteSpace(name) ? L.T("Game controller") : name!, supported));
            }
            return devices;
        }

        /// <summary>The pads' state in coarse form (held buttons, stick positions in eighths). A receiver
        /// stays connected while its pad is switched off, so only a change in this shows the pad is on.</summary>
        public static string State()
        {
            var held = Held();
            var state = new System.Text.StringBuilder(string.Join(",", held.OrderBy(h => h)));
            foreach (var pad in Open.Values)
                for (var axis = 0; axis < 4; axis++)
                    state.Append('|').Append(_axis(pad, axis) / 8192);
            return state.ToString();
        }

        /// <summary>The buttons held now on any pad, as SDL names (a, leftshoulder, …, lefttrigger, righttrigger).</summary>
        public static HashSet<string> Held(bool stick = false)
        {
            var held = new HashSet<string>();
            if (!Start()) return held;
            _update();
            var ids = Ids(_gamepads);
            foreach (var gone in Open.Keys.Except(ids).ToList())
            {
                _close(Open[gone]);
                Open.Remove(gone);
            }
            foreach (var id in ids)
            {
                if (!Open.TryGetValue(id, out var pad))
                {
                    pad = _open(id);
                    if (pad == IntPtr.Zero) continue;
                    Open[id] = pad;
                }
                for (var b = 0; b < ButtonCount; b++)
                    if (_button(pad, b) != 0 && Marshal.PtrToStringUTF8(_buttonName(b)) is { Length: > 0 } name)
                        held.Add(name);
                if (_axis(pad, AxisLeftTrigger) > 16000) held.Add("lefttrigger");
                if (_axis(pad, AxisRightTrigger) > 16000) held.Add("righttrigger");
                if (!stick) continue;
                int x = _axis(pad, 0), y = _axis(pad, 1);
                if (x < -20000) held.Add("leftstick-left");
                if (x > 20000) held.Add("leftstick-right");
                if (y < -20000) held.Add("leftstick-up");
                if (y > 20000) held.Add("leftstick-down");
            }
            return held;
        }
    }
}
