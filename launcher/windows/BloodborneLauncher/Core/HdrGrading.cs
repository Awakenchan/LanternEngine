namespace BloodborneLauncher.Core
{
    /// <summary>
    /// Whether the Native+ chain grades the picture, and the source's stored value as the engine
    /// reads it. Native+ keeps the light of the game's scene in a 16-bit chain and grades it there
    /// (Exposure, Highlights, Shadows, Contrast, Saturation, Bloom); the port's own brightness,
    /// contrast, saturation and bloom are left as the game draws them then.
    /// </summary>
    public static class HdrGrading
    {
        /// <summary>hdr_mode as the engine reads it: "psychov30" is Native+'s name before 0.9.38.2.</summary>
        public static string ModeValue(string stored) => stored == "psychov30" ? "nativeplus" : stored;

        /// <summary>
        /// The engine's startup_nativeplus (bbport_settings.cpp: startup_hdr and the Native+ source),
        /// which is what hdr_grading tests in engine/graphics.cpp. HDR off, or another source, keeps
        /// the port's Picture values in force.
        /// </summary>
        public static bool Active(string hdr, string hdrMode) => hdr == "1" && ModeValue(hdrMode) == "nativeplus";
    }
}
