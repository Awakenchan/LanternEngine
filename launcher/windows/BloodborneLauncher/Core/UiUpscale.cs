using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Text.RegularExpressions;

namespace BloodborneLauncher.Core
{
    public static class UiUpscale
    {
        public sealed record Step(string Text, double Fraction);

        private const string Release = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/";
        private const string Model = "realesrgan-x4plus";
        public const string Credit = "Real-ESRGAN (Xintao Wang et al., BSD 3-Clause) with ncnn (Tencent, BSD 3-Clause)";
        public const string Project = "https://github.com/xinntao/Real-ESRGAN";

        private static (string Zip, long Size, string Sha256, string Exe) Package => OperatingSystem.IsWindows()
            ? ("realesrgan-ncnn-vulkan-20220424-windows.zip", 45474481, "abc02804e17982a3be33675e4d471e91ea374e65b70167abc09e31acb412802d", "realesrgan-ncnn-vulkan.exe")
            : ("realesrgan-ncnn-vulkan-20220424-ubuntu.zip", 46931474, "e5aa6eb131234b87c0c51f82b89390f5e3e642b7b70f2b9bbe95b6a285a40c96", "realesrgan-ncnn-vulkan");

        private static string Assets => Path.Combine(Paths.DataDir, "engine-assets");
        public static string PackDir => Path.Combine(Assets, "ui-4k");
        private static string ToolDir => Path.Combine(Assets, "realesrgan");
        private static string WorkDir => Path.Combine(Assets, "ui-4k-work");
        private static string Marker => Path.Combine(PackDir, "complete");
        private static string Tool => Path.Combine(Paths.BinariesDir, Paths.Program("bb-ui-4k"));

        public static bool Ready => File.Exists(Marker);

        public static bool ToolFound => File.Exists(Tool);

        public static bool WholePng(string path)
        {
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists || info.Length < 57) return false;
                using var file = info.OpenRead();
                Span<byte> head = stackalloc byte[8];
                if (file.Read(head) != 8 || head[0] != 0x89 || head[1] != (byte)'P' || head[2] != (byte)'N' || head[3] != (byte)'G') return false;
                file.Seek(-12, SeekOrigin.End);
                Span<byte> tail = stackalloc byte[12];
                if (file.Read(tail) != 12) return false;
                if (tail[4] == (byte)'I' && tail[5] == (byte)'E' && tail[6] == (byte)'N' && tail[7] == (byte)'D') return true;
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
            return false;
        }

        public static long PackBytes()
        {
            try
            {
                return Directory.Exists(PackDir) ? new DirectoryInfo(PackDir).EnumerateFiles().Sum(f => f.Length) : 0;
            }
            catch (IOException)
            {
                return 0;
            }
        }

        public static void Remove()
        {
            foreach (var dir in new[] { PackDir, WorkDir })
                if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }

        public static async Task BuildAsync(string gameDir, IProgress<Step> progress, CancellationToken cancel, bool half = false)
        {
            var menu = Path.Combine(gameDir, "dvdroot_ps4", "menu");
            if (!Directory.Exists(menu))
                throw new InvalidOperationException(L.T("The game folder has no dvdroot_ps4\\menu: set the game folder first."));
            if (!ToolFound)
                throw new InvalidOperationException(L.T("This engine has no bb-ui-4k tool: update the engine first."));

            var upscaler = await EnsureUpscalerAsync(progress, cancel);
            var source = Path.Combine(WorkDir, "src");
            var upscaled = Path.Combine(WorkDir, "up");
            var todo = Path.Combine(WorkDir, "todo");
            Directory.CreateDirectory(upscaled);

            progress.Report(new Step(L.T("Reading the menu textures from your game files…"), 0));
            await RunAsync(Tool, new[] { "export", menu, source }, null, cancel);

            var all = Directory.GetFiles(source, "*.png").Select(Path.GetFileName).ToList();
            var missing = all.Where(name => !WholePng(Path.Combine(upscaled, name!))).ToList();
            foreach (var name in missing)
            {
                try { File.Delete(Path.Combine(upscaled, name!)); } catch (IOException) { } catch (UnauthorizedAccessException) { }
            }
            if (Directory.Exists(todo)) Directory.Delete(todo, true);
            Directory.CreateDirectory(todo);
            foreach (var name in missing)
                File.Copy(Path.Combine(source, name!), Path.Combine(todo, name!));
            var failure = "";
            if (missing.Count > 0)
            {
                using var watch = new CancellationTokenSource();
                var counting = Task.Run(async () =>
                {
                    while (!watch.IsCancellationRequested)
                    {
                        var done = all.Count(name => WholePng(Path.Combine(upscaled, name!)));
                        progress.Report(new Step(L.F($"Upscaling {done} of {all.Count} textures on the GPU…"), 0.05 + 0.85 * done / all.Count));
                        try { await Task.Delay(1000, watch.Token); } catch (OperationCanceledException) { }
                    }
                });
                try
                {
                    var files = Math.Clamp(Environment.ProcessorCount / 2, 2, 8);
                    try
                    {
                        await RunAsync(upscaler, new[] { "-i", todo, "-o", upscaled, "-n", Model, "-f", "png", "-j", $"{files}:1:{files}" }, Path.GetDirectoryName(upscaler), cancel);
                    }
                    catch (InvalidOperationException e)
                    {
                        failure = e.Message;
                    }
                    var left = all.Where(name => !WholePng(Path.Combine(upscaled, name!))).ToList();
                    if (left.Count > 0)
                    {
                        Directory.Delete(todo, true);
                        Directory.CreateDirectory(todo);
                        foreach (var name in left)
                            File.Copy(Path.Combine(source, name!), Path.Combine(todo, name!));
                        try
                        {
                            await RunAsync(upscaler, new[] { "-i", todo, "-o", upscaled, "-n", Model, "-f", "png", "-t", "128", "-j", "1:1:1" }, Path.GetDirectoryName(upscaler), cancel);
                        }
                        catch (InvalidOperationException e)
                        {
                            failure = e.Message;
                        }
                    }
                }
                finally
                {
                    watch.Cancel();
                    await counting;
                }
            }
            var still = all.Count(name => !WholePng(Path.Combine(upscaled, name!)));
            if (still > 0)
                throw new InvalidOperationException(L.F($"{still} textures were not upscaled. Your GPU driver may not support Vulkan compute, or video memory ran out; try again after a driver update, with other programs closed.") + (failure.Length > 0 ? "\n" + failure : ""));

            if (Directory.Exists(PackDir)) File.Delete(Marker);
            var packed = new Regex(@"^packed (\d+) of (\d+)$");
            await RunAsync(Tool, half ? new[] { "pack", upscaled, PackDir, "half" } : new[] { "pack", upscaled, PackDir }, null, cancel, line =>
            {
                if (packed.Match(line) is { Success: true } m && int.TryParse(m.Groups[1].Value, out var n) && int.TryParse(m.Groups[2].Value, out var total) && total > 0)
                    progress.Report(new Step(L.F($"Packing {n} of {total} textures…"), 0.9 + 0.1 * n / total));
            });
            File.WriteAllText(Marker, $"{all.Count}\n");
            Directory.Delete(WorkDir, true);
            progress.Report(new Step(L.T("4K menus are ready"), 1));
        }

        private static async Task<string> EnsureUpscalerAsync(IProgress<Step> progress, CancellationToken cancel)
        {
            var (zip, size, sha256, exe) = Package;
            var path = Path.Combine(ToolDir, exe);
            var models = Path.Combine(ToolDir, "models");
            if (File.Exists(path) && File.Exists(Path.Combine(models, Model + ".bin")) && File.Exists(Path.Combine(models, Model + ".param")))
                return path;
            Directory.CreateDirectory(ToolDir);
            var download = Path.Combine(ToolDir, zip);
            var report = new Progress<double>(p => progress.Report(new Step(L.F($"Downloading Real-ESRGAN {p:P0}…"), 0.05 * p)));
            await Updater.DownloadAsync(Release + zip, download, size, report, sha256);
            cancel.ThrowIfCancellationRequested();
            using (var archive = ZipFile.OpenRead(download))
            {
                foreach (var entry in archive.Entries)
                {
                    var name = entry.FullName.Replace('\\', '/');
                    var keep = name == exe || name == "vcomp140.dll" || name == $"models/{Model}.bin" || name == $"models/{Model}.param";
                    if (!keep) continue;
                    var target = Path.Combine(ToolDir, name.Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    entry.ExtractToFile(target, true);
                }
            }
            File.Delete(download);
            if (!OperatingSystem.IsWindows())
                File.SetUnixFileMode(path, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            return path;
        }

        private static async Task RunAsync(string program, string[] args, string? workingDir, CancellationToken cancel, Action<string>? line = null)
        {
            var info = new ProcessStartInfo(program)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                WorkingDirectory = workingDir ?? Path.GetDirectoryName(program)!,
            };
            foreach (var arg in args) info.ArgumentList.Add(arg);
            using var process = Process.Start(info) ?? throw new InvalidOperationException(L.F($"{Path.GetFileName(program)} did not start"));
            var errors = new List<string>();
            process.OutputDataReceived += (_, e) => { if (e.Data != null) line?.Invoke(e.Data); };
            process.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (errors) { errors.Add(e.Data); if (errors.Count > 20) errors.RemoveAt(0); } };
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            using (cancel.Register(() => { try { process.Kill(true); } catch (InvalidOperationException) { } }))
                await process.WaitForExitAsync();
            cancel.ThrowIfCancellationRequested();
            if (process.ExitCode != 0)
            {
                string tail;
                lock (errors) tail = string.Join("\n", errors.Where(e => !e.Contains('%')).TakeLast(5));
                throw new InvalidOperationException(L.F($"{Path.GetFileName(program)} stopped (exit {process.ExitCode}). {tail}"));
            }
        }
    }
}
