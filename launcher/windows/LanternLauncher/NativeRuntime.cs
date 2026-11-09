// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO.Compression;
using System.Reflection;
using System.Runtime.InteropServices;

namespace BloodborneLauncher;

internal static class NativeRuntime
{
    private static readonly string[] LoadOrder = { "libSkiaSharp.dll", "libHarfBuzzSharp.dll", "av_libglesv2.dll" };
    private static readonly Dictionary<string, IntPtr> Loaded = new(StringComparer.OrdinalIgnoreCase);

    public static string? Folder { get; private set; }
    public static long UnpackedBytes { get; private set; }
    public static int RemovedOld { get; private set; }

    public static void Prepare()
    {
        if (!OperatingSystem.IsWindows()) return;
        try
        {
            using var stream = typeof(NativeRuntime).Assembly.GetManifestResourceStream("native.zip");
            if (stream == null) return;
            using var zip = new ZipArchive(stream, ZipArchiveMode.Read);
            var entries = zip.Entries.Where(e => e.Name.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)).ToList();
            var key = Key(entries);
            var root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Lantern Engine", "runtime");
            var folder = Path.Combine(root, key);
            Directory.CreateDirectory(folder);
            Folder = folder;
            foreach (var entry in entries)
            {
                var target = Path.Combine(folder, entry.Name);
                if (File.Exists(target) && new FileInfo(target).Length == entry.Length) continue;
                var part = target + "." + Environment.ProcessId + ".part";
                entry.ExtractToFile(part, overwrite: true);
                UnpackedBytes += entry.Length;
                try { File.Move(part, target, overwrite: true); }
                catch (IOException) { File.Delete(part); }
                catch (UnauthorizedAccessException) { File.Delete(part); }
            }
            foreach (var name in LoadOrder.Concat(entries.Select(e => e.Name)).Distinct(StringComparer.OrdinalIgnoreCase))
            {
                var path = Path.Combine(folder, name);
                if (File.Exists(path) && NativeLibrary.TryLoad(path, out var handle))
                    Loaded[Path.GetFileNameWithoutExtension(name)] = handle;
            }
            Resolve(typeof(SkiaSharp.SKObject).Assembly);
            Resolve(typeof(HarfBuzzSharp.Blob).Assembly);
            foreach (var old in Directory.GetDirectories(root).Where(d => !string.Equals(Path.GetFileName(d), key, StringComparison.OrdinalIgnoreCase)))
                try { Directory.Delete(old, recursive: true); RemovedOld++; } catch (Exception) { }
        }
        catch (Exception)
        {
        }
    }

    private static string Key(IEnumerable<ZipArchiveEntry> entries)
    {
        var hash = 14695981039346656037UL;
        foreach (var entry in entries.OrderBy(e => e.Name, StringComparer.OrdinalIgnoreCase))
        {
            foreach (var c in entry.Name.ToLowerInvariant())
                hash = (hash ^ c) * 1099511628211UL;
            hash = (hash ^ (ulong)entry.Length) * 1099511628211UL;
            hash = (hash ^ entry.Crc32) * 1099511628211UL;
        }
        return hash.ToString("x16");
    }

    private static void Resolve(Assembly assembly)
    {
        try
        {
            NativeLibrary.SetDllImportResolver(assembly, (name, _, _) =>
                Loaded.TryGetValue(Path.GetFileNameWithoutExtension(name), out var handle) ? handle : IntPtr.Zero);
        }
        catch (InvalidOperationException)
        {
        }
    }
}
