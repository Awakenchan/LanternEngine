// SPDX-License-Identifier: GPL-3.0-or-later
namespace BloodborneLauncher.Core
{
    public static class EngineFeatures
    {
        public static readonly string[] Keys =
        {
            "legacy_shaders", "exp_vram_cleanup", "exp_frame_pacing", "exp_vram_budget", "exp_thread_priority",
            "exp_sleep_spin", "exp_async_pipelines", "exp_occlusion", "exp_fast_draws", "exp_compile_skip",
            "exp_fast_loading", "exp_hot_pages", "exp_guest_in_place", "exp_native_calls", "exp_sfx_workaround",
            "exp_camera_easing", "fsr4_balanced",
        };

        public static bool Default(string key) => LauncherSettings.Default(key)?.GetValue<bool>() == true;

        public static IEnumerable<string> Changed(LauncherSettings s) => Keys.Where(k => s.Bool(k) != Default(k));

        public static void Reset(LauncherSettings s)
        {
            EnginePreset.Apply(s, EnginePreset.New);
            foreach (var key in Keys)
                s.Set(key, Default(key));
        }
    }
}
