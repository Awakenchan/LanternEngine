// SPDX-License-Identifier: GPL-3.0-or-later
using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;

namespace BloodborneLauncher.Core
{
    public static class GameFiles
    {
        public sealed record Problem(string File, string What);

        public sealed record Result(List<Problem> Problems, int Checked, bool Complete);

        private static readonly string[] Archives =
        {
            "map/m21/gi_env_m21.tpfbhd", "map/m21/m21_0000.tpfbhd", "map/m21/m21_0001.tpfbhd", "map/m21/m21_0002.tpfbhd", "map/m21/m21_0003.tpfbhd",
            "map/m21_00_00_00/h21_00_00_00.hkxbhd", "map/m21_00_00_00/l21_00_00_00.hkxbhd", "map/m21_01_00_00/h21_01_00_00.hkxbhd",
            "map/m21_01_00_00/l21_01_00_00.hkxbhd", "map/m22/gi_env_m22.tpfbhd", "map/m22/m22_0000.tpfbhd", "map/m22/m22_0001.tpfbhd",
            "map/m22/m22_0002.tpfbhd", "map/m22/m22_0003.tpfbhd", "map/m22_00_00_00/h22_00_00_00.hkxbhd", "map/m22_00_00_00/l22_00_00_00.hkxbhd",
            "map/m23/gi_env_m23.tpfbhd", "map/m23/m23_0000.tpfbhd", "map/m23/m23_0001.tpfbhd", "map/m23/m23_0002.tpfbhd", "map/m23/m23_0003.tpfbhd",
            "map/m23_00_00_00/h23_00_00_00.hkxbhd", "map/m23_00_00_00/l23_00_00_00.hkxbhd", "map/m24/gi_env_m24.tpfbhd",
            "map/m24/gi_env_m24_01.tpfbhd", "map/m24/m24_0000.tpfbhd", "map/m24/m24_0001.tpfbhd", "map/m24/m24_0002.tpfbhd",
            "map/m24/m24_0003.tpfbhd", "map/m24/m24_01_0000.tpfbhd", "map/m24/m24_01_0001.tpfbhd", "map/m24/m24_01_0002.tpfbhd",
            "map/m24/m24_01_0003.tpfbhd", "map/m24_00_00_00/h24_00_00_00.hkxbhd", "map/m24_00_00_00/l24_00_00_00.hkxbhd",
            "map/m24_01_00_00/h24_01_00_00.hkxbhd", "map/m24_01_00_00/l24_01_00_00.hkxbhd", "map/m24_02_00_00/h24_02_00_00.hkxbhd",
            "map/m24_02_00_00/l24_02_00_00.hkxbhd", "map/m25/gi_env_m25.tpfbhd", "map/m25/m25_0000.tpfbhd", "map/m25/m25_0001.tpfbhd",
            "map/m25/m25_0002.tpfbhd", "map/m25/m25_0003.tpfbhd", "map/m25_00_00_00/h25_00_00_00.hkxbhd", "map/m25_00_00_00/l25_00_00_00.hkxbhd",
            "map/m26/gi_env_m26.tpfbhd", "map/m26/m26_0000.tpfbhd", "map/m26/m26_0001.tpfbhd", "map/m26/m26_0002.tpfbhd", "map/m26/m26_0003.tpfbhd",
            "map/m26_00_00_00/h26_00_00_00.hkxbhd", "map/m26_00_00_00/l26_00_00_00.hkxbhd", "map/m27/gi_env_m27.tpfbhd", "map/m27/m27_0000.tpfbhd",
            "map/m27/m27_0001.tpfbhd", "map/m27/m27_0002.tpfbhd", "map/m27/m27_0003.tpfbhd", "map/m27_00_00_00/h27_00_00_00.hkxbhd",
            "map/m27_00_00_00/l27_00_00_00.hkxbhd", "map/m28/gi_env_m28.tpfbhd", "map/m28/m28_0000.tpfbhd", "map/m28/m28_0001.tpfbhd",
            "map/m28/m28_0002.tpfbhd", "map/m28/m28_0003.tpfbhd", "map/m28_00_00_00/h28_00_00_00.hkxbhd", "map/m28_00_00_00/l28_00_00_00.hkxbhd",
            "map/m29/m29_0000.tpfbhd", "map/m29/m29_0001.tpfbhd", "map/m29/m29_0002.tpfbhd", "map/m29/m29_0003.tpfbhd",
            "map/m29_00_00_00/h29_00_00_00.hkxbhd", "map/m29_00_00_00/l29_00_00_00.hkxbhd", "map/m32/gi_env_m32.tpfbhd", "map/m32/m32_0000.tpfbhd",
            "map/m32/m32_0001.tpfbhd", "map/m32/m32_0002.tpfbhd", "map/m32/m32_0003.tpfbhd", "map/m32_00_00_00/h32_00_00_00.hkxbhd",
            "map/m32_00_00_00/l32_00_00_00.hkxbhd", "map/m33/gi_env_m33.tpfbhd", "map/m33/m33_0000.tpfbhd", "map/m33/m33_0001.tpfbhd",
            "map/m33/m33_0002.tpfbhd", "map/m33/m33_0003.tpfbhd", "map/m33_00_00_00/h33_00_00_00.hkxbhd", "map/m33_00_00_00/l33_00_00_00.hkxbhd",
        };

        public static Result Check(string gameDir, string? updateDir, TimeSpan budget)
        {
            var problems = new List<Problem>();
            var roots = new[] { updateDir, gameDir }.Where(d => !string.IsNullOrEmpty(d))
                .Select(d => Path.Combine(d!, "dvdroot_ps4")).Where(Directory.Exists).ToList();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            var files = new List<(string Relative, FileInfo Info)>();
            var options = new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true };
            foreach (var root in roots)
            {
                try
                {
                    foreach (var info in new DirectoryInfo(root).EnumerateFiles("*", options))
                    {
                        var relative = Path.GetRelativePath(root, info.FullName);
                        if (seen.Add(relative)) files.Add((relative, info));
                    }
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }

            foreach (var (relative, info) in files)
            {
                if (info.Length == 0 && !relative.EndsWith(".patch", StringComparison.OrdinalIgnoreCase))
                    problems.Add(new Problem(relative, L.T("is empty (0 bytes)")));
                if (!relative.EndsWith("bhd", StringComparison.OrdinalIgnoreCase)) continue;
                var data = relative[..^3] + "bdt";
                var found = files.FirstOrDefault(f => string.Equals(f.Relative, data, StringComparison.OrdinalIgnoreCase)).Info;
                if (found == null)
                {
                    problems.Add(new Problem(data, L.T("is missing")));
                    continue;
                }
                var need = ArchiveLength(info);
                if (need > found.Length)
                    problems.Add(new Problem(data, L.F($"is cut short ({found.Length:N0} of {need:N0} bytes)")));
            }

            if (files.Count > 0)
                foreach (var header in Archives)
                    foreach (var name in new[] { header, header[..^3] + "bdt" })
                    {
                        var relative = name.Replace('/', Path.DirectorySeparatorChar);
                        if (!seen.Contains(relative) && problems.All(p => !string.Equals(p.File, relative, StringComparison.OrdinalIgnoreCase)))
                            problems.Add(new Problem(relative, L.T("is missing")));
                    }

            var clock = Stopwatch.StartNew();
            var packed = files.Where(f => f.Info.Length > 0 && f.Relative.EndsWith(".dcx", StringComparison.OrdinalIgnoreCase)).ToList();
            var done = 0;
            var head = new byte[0x24];
            foreach (var (relative, info) in packed)
            {
                if (clock.Elapsed > budget) break;
                done++;
                try
                {
                    using var stream = new FileStream(info.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 4096);
                    if (stream.Read(head, 0, head.Length) < head.Length)
                    {
                        problems.Add(new Problem(relative, L.T("is cut short")));
                        continue;
                    }
                    if (head[0] != 'D' || head[1] != 'C' || head[2] != 'X' || head[3] != 0) continue;
                    long need = BinaryPrimitives.ReadUInt32BigEndian(head.AsSpan(0x20)) + 76L;
                    if (info.Length < need)
                        problems.Add(new Problem(relative, L.F($"is cut short ({info.Length:N0} of {need:N0} bytes)")));
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    problems.Add(new Problem(relative, L.T("cannot be read: ") + e.Message));
                }
            }
            return new Result(problems, done, done == packed.Count);
        }

        private static long ArchiveLength(FileInfo header)
        {
            try
            {
                var b = File.ReadAllBytes(header.FullName);
                if (b.Length < 0x40 || b[0] != 'B' || b[1] != 'H' || b[2] != 'F' || b[3] != '4' || b[9] != 0) return 0;
                var count = BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(0x0c));
                var start = BinaryPrimitives.ReadInt64LittleEndian(b.AsSpan(0x10));
                var entry = BinaryPrimitives.ReadInt64LittleEndian(b.AsSpan(0x20));
                if (entry != 0x24 || start < 0x40 || count < 0 || start + count * entry > b.Length) return 0;
                long need = 0;
                for (var i = 0; i < count; i++)
                {
                    var at = (int)(start + i * entry);
                    var size = BinaryPrimitives.ReadInt64LittleEndian(b.AsSpan(at + 8));
                    long offset = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(at + 0x18));
                    if (size > 0) need = Math.Max(need, offset + size);
                }
                return need;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return 0;
            }
        }
    }
}
