// SPDX-License-Identifier: GPL-3.0-or-later
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;

namespace BloodborneLauncher.Core
{
    [GeneratedComInterface(StringMarshalling = StringMarshalling.Utf16)]
    [Guid("000214F9-0000-0000-C000-000000000046")]
    internal unsafe partial interface IShellLinkW
    {
        void GetPath(char* file, int max, IntPtr data, uint flags);
        void GetIDList(out IntPtr list);
        void SetIDList(IntPtr list);
        void GetDescription(char* name, int max);
        void SetDescription(string name);
        void GetWorkingDirectory(char* dir, int max);
        void SetWorkingDirectory(string dir);
        void GetArguments(char* args, int max);
        void SetArguments(string args);
        void GetHotkey(out ushort hotkey);
        void SetHotkey(ushort hotkey);
        void GetShowCmd(out int show);
        void SetShowCmd(int show);
        void GetIconLocation(char* path, int max, out int index);
        void SetIconLocation(string path, int index);
        void SetRelativePath(string path, uint reserved);
        void Resolve(IntPtr window, uint flags);
        void SetPath(string file);
    }

    [GeneratedComInterface(StringMarshalling = StringMarshalling.Utf16)]
    [Guid("0000010b-0000-0000-C000-000000000046")]
    internal partial interface IPersistFile
    {
        void GetClassID(out Guid id);
        [PreserveSig] int IsDirty();
        void Load(string file, uint mode);
        void Save(string? file, [MarshalAs(UnmanagedType.Bool)] bool remember);
        void SaveCompleted(string file);
        void GetCurFile(out IntPtr file);
    }

#if AVALONIA
    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
#endif
    internal static unsafe partial class ShellLinks
    {
        private static readonly Guid ShellLinkClass = new("00021401-0000-0000-C000-000000000046");
        private static readonly Guid UnknownInterface = new("00000000-0000-0000-C000-000000000046");
        private static readonly StrategyBasedComWrappers Wrappers = new();

        [LibraryImport("ole32.dll")]
        private static partial int CoCreateInstance(in Guid clsid, IntPtr outer, uint context, in Guid iid, out IntPtr instance);

        public sealed record Link(string Target, string WorkingDirectory, string IconPath, int IconIndex);

        private static IShellLinkW? Open(string file, bool write)
        {
            if (CoCreateInstance(ShellLinkClass, IntPtr.Zero, 1, UnknownInterface, out var unknown) < 0 || unknown == IntPtr.Zero)
                return null;
            try
            {
                var link = (IShellLinkW)Wrappers.GetOrCreateObjectForComInstance(unknown, CreateObjectFlags.UniqueInstance);
                ((IPersistFile)link).Load(file, write ? 2u : 0u);
                return link;
            }
            finally
            {
                Marshal.Release(unknown);
            }
        }

        public static Link? Read(string file)
        {
            var link = Open(file, write: false);
            if (link == null) return null;
            var buffer = stackalloc char[1024];
            link.GetPath(buffer, 1024, IntPtr.Zero, 4);
            var target = new string(buffer);
            buffer[0] = '\0';
            link.GetWorkingDirectory(buffer, 1024);
            var directory = new string(buffer);
            buffer[0] = '\0';
            link.GetIconLocation(buffer, 1024, out var index);
            return new Link(target, directory, new string(buffer), index);
        }

        public static bool Change(string file, Action<IShellLinkW> change)
        {
            var link = Open(file, write: true);
            if (link == null) return false;
            change(link);
            ((IPersistFile)link).Save(null, true);
            return true;
        }

        public static string? TargetOf(string file)
        {
            try { return Read(file)?.Target is { Length: > 0 } target ? Path.GetFullPath(target) : null; }
            catch (Exception) { return null; }
        }
    }
}
