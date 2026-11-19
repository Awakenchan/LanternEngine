// SPDX-License-Identifier: GPL-3.0-or-later
using System.Linq;

namespace BloodborneLauncher.Core
{
    public static class SeamlessPlay
    {
        public const int RoomLength = 20;
        public const int PasswordLength = 24;

        public static string RandomText(int length)
        {
            const string letters = "abcdefghjkmnpqrstuvwxyz23456789";
            var bytes = System.Security.Cryptography.RandomNumberGenerator.GetBytes(length);
            return new string(bytes.Select(b => letters[b % letters.Length]).ToArray());
        }

        public static bool HostRoomReady(string hostRoom, string hostPassword) =>
            hostRoom.Length > 0 && hostPassword.Length > 0;

        public static bool NeedsHostRoom(bool host, bool direct, string hostRoom, string hostPassword) =>
            host && !direct && !HostRoomReady(hostRoom, hostPassword);

        public static bool Missing(bool host, bool direct, string room, string hostRoom, string hostPassword, string directHost) =>
            direct ? !host && directHost.Length == 0
            : host ? !HostRoomReady(hostRoom, hostPassword)
            : room.Length == 0;
    }
}
