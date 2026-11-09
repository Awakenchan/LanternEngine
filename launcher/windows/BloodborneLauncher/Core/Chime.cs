using System.IO;
using System.Media;

namespace BloodborneLauncher.Core
{
    /// <summary>
    /// The trophy sound: a short two-note bell synthesised here (so no sound file has to ship), or
    /// a .wav of your own. Volume scales the samples, since SoundPlayer has no volume of its own.
    /// </summary>
    public static class Chime
    {
        private const int Rate = 44100;

        public static void Play(int volumePercent, string customFile = "")
        {
#if AVALONIA
            if (!OperatingSystem.IsWindows())
            {
                PlayLinux(volumePercent, customFile);
                return;
            }
            PlayWindows(volumePercent, customFile);
        }

        /// <summary>Linux: the .wav through the desktop's sound server (PipeWire, PulseAudio) or ALSA.</summary>
        private static void PlayLinux(int volumePercent, string customFile)
        {
            try
            {
                var volume = Math.Clamp(volumePercent, 0, 100);
                var file = customFile.Length > 0 && File.Exists(customFile) ? customFile : null;
                if (file != null && Scaled(file, volume / 100.0) is { } scaled)
                {
                    file = Path.Combine(Path.GetTempPath(), "lantern-chime-custom.wav");
                    File.WriteAllBytes(file, scaled);
                }
                if (file == null)
                {
                    file = Path.Combine(Path.GetTempPath(), $"lantern-chime-{volume}.wav");
                    if (!File.Exists(file)) File.WriteAllBytes(file, Wave(volume / 100.0));
                }
                // MP3, MP4/M4A: a media player that decodes them, at the volume; a .wav (ours, or
                // the player's at the volume above) through the sound server.
                var compressed = Scaled(file, 1.0) == null;
                var players = compressed
                    ? new[] { "ffplay", "mpv", "gst-play-1.0", "pw-play", "paplay" }
                    : new[] { "pw-play", "paplay", "aplay" };
                foreach (var player in players)
                {
                    try
                    {
                        var info = new System.Diagnostics.ProcessStartInfo(player) { UseShellExecute = false, RedirectStandardError = true, RedirectStandardOutput = true };
                        if (player == "aplay") info.ArgumentList.Add("-q");
                        if (compressed)
                            foreach (var arg in player switch
                            {
                                "ffplay" => new[] { "-nodisp", "-autoexit", "-loglevel", "quiet", "-volume", volume.ToString() },
                                "mpv" => new[] { "--no-video", "--really-quiet", $"--volume={volume}" },
                                "gst-play-1.0" => new[] { "--quiet", $"--volume={volume / 100.0:0.00}" },
                                "pw-play" => new[] { $"--volume={volume / 100.0:0.00}" },
                                _ => new[] { $"--volume={volume * 65536 / 100}" },
                            })
                                info.ArgumentList.Add(arg);
                        info.ArgumentList.Add(file);
                        System.Diagnostics.Process.Start(info)?.Dispose();
                        return;
                    }
                    catch (System.ComponentModel.Win32Exception)
                    {
                        // Not installed: the next one.
                    }
                }
            }
            catch
            {
                // No audio: the notification still shows.
            }
        }

        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
        private static void PlayWindows(int volumePercent, string customFile)
        {
#endif
            try
            {
                if (customFile.Length > 0 && File.Exists(customFile))
                {
                    // Read here, at the volume: the system player takes 16-bit PCM only (24-bit
                    // and float files stayed silent) and has no volume. Compressed ones as they are.
                    var scaled = Scaled(customFile, Math.Clamp(volumePercent, 0, 100) / 100.0);
#if !AVALONIA
                    // MP3, MP4/M4A, WMA...: Windows' media player, which has a volume.
                    if (scaled == null)
                    {
                        PlayMedia(customFile, Math.Clamp(volumePercent, 0, 100) / 100.0);
                        return;
                    }
#else
                    if (scaled == null)
                    {
                        var volume = Math.Clamp(volumePercent, 0, 100) * 10;
                        var file = Path.GetFullPath(customFile);
                        Avalonia.Threading.Dispatcher.UIThread.Post(() => PlayMci(file, volume));
                        return;
                    }
#endif
                    (scaled != null ? new SoundPlayer(new MemoryStream(scaled)) : new SoundPlayer(customFile)).Play();
                    return;
                }
                var player = new SoundPlayer(new MemoryStream(Wave(Math.Clamp(volumePercent, 0, 100) / 100.0)));
                player.Play();
            }
            catch
            {
                // No audio device: the notification still shows.
            }
        }

#if AVALONIA
        [System.Runtime.InteropServices.DllImport("winmm.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, EntryPoint = "mciSendStringW")]
        private static extern int MciSend(string command, IntPtr result, int length, IntPtr callback);

        [System.Runtime.Versioning.SupportedOSPlatform("windows")]
        private static void PlayMci(string file, int volume)
        {
            try
            {
                MciSend("close lanternchime", IntPtr.Zero, 0, IntPtr.Zero);
                if (MciSend($"open \"{file}\" type mpegvideo alias lanternchime", IntPtr.Zero, 0, IntPtr.Zero) != 0) return;
                MciSend($"setaudio lanternchime volume to {volume}", IntPtr.Zero, 0, IntPtr.Zero);
                MciSend("play lanternchime", IntPtr.Zero, 0, IntPtr.Zero);
            }
            catch
            {
            }
        }
#endif

#if !AVALONIA
        // Kept until the sound ends: a collected player stops.
        private static System.Windows.Media.MediaPlayer? _media;

        /// <summary>A sound file Windows' Media Foundation decodes (MP3, MP4/M4A, AAC, WMA), at
        /// <paramref name="volume"/> (0 to 1), on the UI thread (the player needs its dispatcher).</summary>
        private static void PlayMedia(string file, double volume)
        {
            void Start()
            {
                _media?.Close();
                var player = new System.Windows.Media.MediaPlayer { Volume = volume };
                void Done(object? sender, EventArgs e)
                {
                    player.Close();
                    if (_media == player) _media = null;
                }
                player.MediaEnded += Done;
                player.MediaFailed += (sender, e) => Done(sender, e);
                player.Open(new Uri(Path.GetFullPath(file)));
                player.Play();
                _media = player;
            }
            var dispatcher = System.Windows.Application.Current?.Dispatcher;
            if (dispatcher == null || dispatcher.CheckAccess()) Start();
            else dispatcher.BeginInvoke((Action)Start);
        }
#endif

        /// <summary>A .wav of the player's as 16-bit PCM at `volume` (0 to 1): integer samples of 8
        /// to 32 bits or float ones, plain or WAVE_FORMAT_EXTENSIBLE, any rate, the first two
        /// channels. Null for what it cannot read (compressed formats).</summary>
        internal static byte[]? Scaled(string file, double volume)
        {
            try
            {
                var data = File.ReadAllBytes(file);
                if (data.Length < 12 || System.Text.Encoding.ASCII.GetString(data, 0, 4) != "RIFF" ||
                    System.Text.Encoding.ASCII.GetString(data, 8, 4) != "WAVE")
                    return null;
                int format = 0, channels = 0, rate = 0, bits = 0, at = -1, length = 0;
                for (var pos = 12; pos + 8 <= data.Length;)
                {
                    var id = System.Text.Encoding.ASCII.GetString(data, pos, 4);
                    var size = BitConverter.ToUInt32(data, pos + 4);
                    var body = pos + 8;
                    if (id == "fmt " && size >= 16 && body + 16 <= data.Length)
                    {
                        format = BitConverter.ToUInt16(data, body);
                        channels = BitConverter.ToUInt16(data, body + 2);
                        rate = BitConverter.ToInt32(data, body + 4);
                        bits = BitConverter.ToUInt16(data, body + 14);
                        // WAVE_FORMAT_EXTENSIBLE: the sub-format GUID starts with the format tag.
                        if (format == 0xFFFE && size >= 40 && body + 26 <= data.Length)
                            format = BitConverter.ToUInt16(data, body + 24);
                    }
                    else if (id == "data")
                    {
                        // Writers that stream leave the size 0 or 0xFFFFFFFF: to the file's end.
                        at = body;
                        length = size == 0 || size > data.Length - body ? data.Length - body : (int)size;
                        break;
                    }
                    if (size > data.Length) break;
                    pos = body + (int)size + (int)(size & 1);
                }
                var pcm = format == 1 && bits is 8 or 16 or 24 or 32;
                var floats = format == 3 && bits is 32 or 64;
                if (at < 0 || channels <= 0 || rate <= 0 || (!pcm && !floats)) return null;
                var width = bits / 8;
                var frames = length / (width * channels);
                var kept = Math.Min(channels, 2);
                using var memory = new MemoryStream();
                using var writer = new BinaryWriter(memory);
                writer.Write("RIFF"u8);
                writer.Write(36 + frames * kept * 2);
                writer.Write("WAVEfmt "u8);
                writer.Write(16);
                writer.Write((short)1);
                writer.Write((short)kept);
                writer.Write(rate);
                writer.Write(rate * kept * 2);
                writer.Write((short)(kept * 2));
                writer.Write((short)16);
                writer.Write("data"u8);
                writer.Write(frames * kept * 2);
                for (var f = 0; f < frames; f++)
                    for (var c = 0; c < kept; c++)
                    {
                        var i = at + (f * channels + c) * width;
                        double s = floats
                            ? (bits == 32 ? BitConverter.ToSingle(data, i) : BitConverter.ToDouble(data, i))
                            : bits switch
                            {
                                8 => (data[i] - 128) / 128.0,
                                16 => BitConverter.ToInt16(data, i) / 32768.0,
                                24 => (data[i] | data[i + 1] << 8 | (sbyte)data[i + 2] << 16) / 8388608.0,
                                _ => BitConverter.ToInt32(data, i) / 2147483648.0,
                            };
                        writer.Write((short)Math.Clamp(s * volume * short.MaxValue, short.MinValue, short.MaxValue));
                    }
                writer.Flush();
                return memory.ToArray();
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>16-bit mono PCM: a soft low note, then a brighter one, each a bell (fundamental
        /// plus decaying inharmonic partials).</summary>
        private static byte[] Wave(double volume)
        {
            var seconds = 1.6;
            var count = (int)(Rate * seconds);
            var samples = new double[count];
            Bell(samples, 0.00, 659.25, 0.55);  // E5
            Bell(samples, 0.14, 987.77, 0.65);  // B5

            var peak = samples.Max(Math.Abs);
            using var memory = new MemoryStream();
            using var writer = new BinaryWriter(memory);
            writer.Write("RIFF"u8);
            writer.Write(36 + count * 2);
            writer.Write("WAVEfmt "u8);
            writer.Write(16);
            writer.Write((short)1);
            writer.Write((short)1);
            writer.Write(Rate);
            writer.Write(Rate * 2);
            writer.Write((short)2);
            writer.Write((short)16);
            writer.Write("data"u8);
            writer.Write(count * 2);
            var gain = peak > 0 ? 0.8 * volume / peak : 0;
            foreach (var s in samples)
                writer.Write((short)Math.Clamp(s * gain * short.MaxValue, short.MinValue, short.MaxValue));
            writer.Flush();
            return memory.ToArray();
        }

        private static void Bell(double[] samples, double start, double frequency, double amplitude)
        {
            var partials = new (double Ratio, double Level, double Decay)[]
            {
                (1.0, 1.0, 2.6), (2.0, 0.35, 4.0), (2.76, 0.18, 6.0), (5.4, 0.06, 9.0),
            };
            var first = (int)(start * Rate);
            for (var i = first; i < samples.Length; i++)
            {
                var t = (i - first) / (double)Rate;
                var attack = Math.Min(1.0, t / 0.006);
                var value = 0.0;
                foreach (var (ratio, level, decay) in partials)
                    value += level * Math.Exp(-decay * t) * Math.Sin(2 * Math.PI * frequency * ratio * t);
                samples[i] += amplitude * attack * value;
            }
        }
    }
}
