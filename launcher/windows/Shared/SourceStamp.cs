using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace BloodborneLauncher.Build
{
    /// <summary>
    /// The fingerprint of a project's sources, shared by the builder (which writes it beside the
    /// exes it installs) and the launcher (which compares it with the sources on disk to say when
    /// it is out of date). Both must hash exactly the same files the same way, hence one file.
    ///
    /// Layout: &lt;repo&gt;/launcher/windows/{BloodborneLauncher,LauncherBuilder,Shared} are the
    /// sources; &lt;repo&gt;/LauncherApp holds the installed exes and build-stamp.json.
    /// </summary>
    public static class SourceStamp
    {
        public const string LauncherProject = "BloodborneLauncher";
        public const string BuilderProject = "LauncherBuilder";
        public const string SharedFolder = "Shared";
        public const string AppFolder = "LauncherApp";
        public const string StampName = "build-stamp.json";
        public const string LauncherExe = "Bloodborne Launcher.exe";
        public const string BuilderExe = "Build Launcher.exe";

        private static readonly HashSet<string> SkippedFolders = new(StringComparer.OrdinalIgnoreCase)
        {
            "bin", "obj", ".vs", ".idea"
        };

        private static readonly HashSet<string> SkippedExtensions = new(StringComparer.OrdinalIgnoreCase)
        {
            ".user", ".suo", ".swp", ".tmp"
        };

        /// <summary>launcher/windows inside the repo, found by walking up from `start`.</summary>
        public static string? FindSourceRoot(string start)
        {
            for (var dir = new DirectoryInfo(start); dir != null; dir = dir.Parent)
            {
                var candidate = Path.Combine(dir.FullName, "launcher", "windows");
                if (File.Exists(Path.Combine(candidate, LauncherProject, LauncherProject + ".csproj")))
                    return candidate;
            }
            return null;
        }

        /// <summary>The repository folder (bloodborne_pc-master) above launcher/windows.</summary>
        public static string RepoOf(string sourceRoot) =>
            Directory.GetParent(Directory.GetParent(sourceRoot)!.FullName)!.FullName;

        /// <summary>Where the builder installs: the folder named in launcher/windows/install.txt (the
        /// Bloodborne PC package), else &lt;repo&gt;/LauncherApp.</summary>
        public static string AppDirOf(string sourceRoot)
        {
            var file = Path.Combine(sourceRoot, "install.txt");
            if (File.Exists(file) && File.ReadAllText(file).Trim() is { Length: > 0 } dir)
                return Environment.ExpandEnvironmentVariables(dir);
            return Path.Combine(RepoOf(sourceRoot), AppFolder);
        }

        /// <summary>The sources: above `start`, or the ones the stamp beside `start` was built from
        /// (an installed copy outside the checkout).</summary>
        public static string? LocateSources(string start)
        {
            if (FindSourceRoot(start) is { } root) return root;
            var recorded = Read(start).Source;
            return recorded.Length > 0 && File.Exists(Path.Combine(recorded, LauncherProject, LauncherProject + ".csproj"))
                ? recorded
                : null;
        }

        /// <summary>SHA-256 over the relative path and bytes of every source file of `project`
        /// and the shared folder, in a stable order. Line endings are normalised so a checkout
        /// with different autocrlf settings does not count as a change.</summary>
        public static string Compute(string sourceRoot, string project)
        {
            using var sha = SHA256.Create();
            foreach (var folder in new[] { project, SharedFolder })
            {
                var root = Path.Combine(sourceRoot, folder);
                if (!Directory.Exists(root))
                    continue;

                foreach (var file in Files(root).OrderBy(f => f, StringComparer.Ordinal))
                {
                    var relative = folder + "/" + Path.GetRelativePath(root, file).Replace('\\', '/');
                    var name = Encoding.UTF8.GetBytes(relative + "\n");
                    sha.TransformBlock(name, 0, name.Length, null, 0);
                    var bytes = File.ReadAllBytes(file);
                    var text = Normalise(bytes);
                    sha.TransformBlock(text, 0, text.Length, null, 0);
                }
            }
            sha.TransformFinalBlock(Array.Empty<byte>(), 0, 0);
            return Convert.ToHexString(sha.Hash!).ToLowerInvariant();
        }

        private static IEnumerable<string> Files(string root)
        {
            foreach (var file in Directory.EnumerateFiles(root))
                if (!SkippedExtensions.Contains(Path.GetExtension(file)))
                    yield return file;

            foreach (var dir in Directory.EnumerateDirectories(root))
            {
                if (SkippedFolders.Contains(Path.GetFileName(dir)))
                    continue;
                foreach (var file in Files(dir))
                    yield return file;
            }
        }

        private static byte[] Normalise(byte[] bytes)
        {
            var output = new List<byte>(bytes.Length);
            for (var i = 0; i < bytes.Length; i++)
                if (!(bytes[i] == '\r' && i + 1 < bytes.Length && bytes[i + 1] == '\n'))
                    output.Add(bytes[i]);
            return output.ToArray();
        }

        // ---------------------------------------------------------------- stamp file

        public sealed class Stamp
        {
            public string Launcher { get; set; } = "";
            public string Builder { get; set; } = "";
            public string Source { get; set; } = "";
            public DateTime Built { get; set; }
        }

        public static Stamp Read(string appDir)
        {
            try
            {
                var path = Path.Combine(appDir, StampName);
                if (!File.Exists(path)) return new Stamp();
                using var doc = JsonDocument.Parse(File.ReadAllText(path));
                var root = doc.RootElement;
                string Text(string name) => root.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
                return new Stamp
                {
                    Launcher = Text(nameof(Stamp.Launcher)),
                    Builder = Text(nameof(Stamp.Builder)),
                    Source = Text(nameof(Stamp.Source)),
                    Built = root.TryGetProperty(nameof(Stamp.Built), out var b) && b.TryGetDateTime(out var built) ? built : default,
                };
            }
            catch
            {
                return new Stamp();
            }
        }

        public static void Write(string appDir, Stamp stamp)
        {
            using var buffer = new MemoryStream();
            using (var json = new Utf8JsonWriter(buffer, new JsonWriterOptions { Indented = true }))
            {
                json.WriteStartObject();
                json.WriteString(nameof(Stamp.Launcher), stamp.Launcher);
                json.WriteString(nameof(Stamp.Builder), stamp.Builder);
                json.WriteString(nameof(Stamp.Source), stamp.Source);
                json.WriteString(nameof(Stamp.Built), stamp.Built);
                json.WriteEndObject();
            }
            File.WriteAllBytes(Path.Combine(appDir, StampName), buffer.ToArray());
        }
    }
}
