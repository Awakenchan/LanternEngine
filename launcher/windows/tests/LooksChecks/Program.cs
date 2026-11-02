using System.IO;
using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

static bool Refused(string text)
{
    try
    {
        Looks.Parse(text);
        return false;
    }
    catch (FormatException)
    {
        return true;
    }
}

var knobs = Looks.Knobs.Select(k => k.Key).ToHashSet();
Require(knobs.Count == Looks.Knobs.Length, "A key is listed twice");
Require(knobs.All(k => k.StartsWith("graphics_") || k.StartsWith("effect_")), "A knob that is not a graphics value");
foreach (var knob in Looks.Knobs)
{
    Require(Looks.Normal(knob, knob.Default) != null, knob.Key + ": bad default");
    Require(knob.Min < knob.Max, knob.Key + ": bad range");
}
foreach (var look in Looks.Presets)
{
    Require(look.BuiltIn && look.Description.Length > 0, look.Name + ": a preset says what it costs");
    Require(look.Values.Keys.All(knobs.Contains), look.Name + ": a preset sets a key outside the list");
}

var values = new Dictionary<string, string> { ["effect_gi"] = "1", ["graphics_gi_strength"] = "7.5" };
var original = new Looks.Look("Mine", "Someone", values);
var fromJson = Looks.Parse(Looks.ToJson(original));
var fromCode = Looks.Parse("here it is: " + Looks.ToCode(original) + " enjoy");
foreach (var parsed in new[] { fromJson, fromCode })
{
    Require(parsed.Name == "Mine" && parsed.Author == "Someone", "Name or author lost");
    Require(parsed.Values.Count == 2 && parsed.Values["graphics_gi_strength"] == "7.5", "Values lost");
    Require(!parsed.BuiltIn, "An import must not read as a preset");
}

const string Good = "{\"lantern_look\":1,\"name\":\"x\",\"author\":\"y\",\"values\":{\"effect_gi\":\"1\"}}";
Require(!Refused(Good), "A good look was refused");
Require(Refused(Good.Replace("effect_gi", "BB_USER_DIR")), "An env variable got through");
Require(Refused(Good.Replace("effect_gi", "upscaler")), "A setting outside the list got through");
Require(Refused(Good.Replace("effect_gi", "exp_guest_in_place")), "An engine switch got through");
Require(Refused(Good.Replace("effect_gi", "../../saves")), "A path got through");
Require(Refused(Good.Replace("\"lantern_look\":1", "\"lantern_look\":2")), "A newer version got through");
Require(Refused(Good.Replace("\"name\":\"x\"", "\"run\":\"calc.exe\"")), "An unknown field got through");
Require(Refused(Good.Replace("\"1\"", "\"NaN\"")), "NaN got through");
Require(Refused(Good.Replace("\"1\"", "\"Infinity\"")), "Infinity got through");
Require(Refused(Good.Replace("\"1\"", "\"lots\"")), "Text got through");
Require(Refused(Good.Replace("\"1\"", "{\"a\":1}")), "An object got through");
Require(Refused("not a look"), "Plain text got through");
Require(Refused("{"), "Broken JSON got through");
Require(Refused(new string(' ', 64 * 1024) + Good), "A huge text got through");
Require(Refused(Looks.CodePrefix + "!!!!"), "A broken code got through");

var clampHigh = Looks.Parse(Good.Replace("effect_gi\":\"1\"", "graphics_gi_strength\":\"999\""));
Require(clampHigh.Values["graphics_gi_strength"] == "12", "No clamp above");
var clampLow = Looks.Parse(Good.Replace("effect_gi\":\"1\"", "graphics_contrast\":-5"));
Require(clampLow.Values["graphics_contrast"] == "0.5", "No clamp below");
var whole = Looks.Parse(Good.Replace("effect_gi\":\"1\"", "graphics_shadow_resolution\":\"7\""));
Require(whole.Values["graphics_shadow_resolution"] == "2", "No clamp of a level");

using (var packed = new MemoryStream())
{
    using (var deflate = new System.IO.Compression.DeflateStream(packed, System.IO.Compression.CompressionLevel.SmallestSize))
        deflate.Write(new byte[4 * 1024 * 1024]);
    var bomb = Looks.CodePrefix + Convert.ToBase64String(packed.ToArray()).TrimEnd('=').Replace('+', '-').Replace('/', '_');
    Require(Refused(bomb), "A zip bomb got through");
}

var odd = Looks.Parse(Good.Replace("\"x\"", "\"a\\u202Eb\\u0007c" + new string('n', 200) + "\""));
Require(odd.Name.Length <= 48 && !odd.Name.Contains('‮') && !odd.Name.Contains('\a'), "A name was not cleaned");

var steady = EngineSwitches.All.SingleOrDefault(s => s.Env == "BB_VOLUMETRIC_STEADY");
Require(steady != null, "The steady sunlight color switch is missing from the Engine page");
Require(steady!.Group == EngineSwitches.Graphics, "The steady sunlight color switch is not in the Graphics group");
Require(steady.Kind == SwitchKind.Toggle && steady.Default == steady.On, "The steady sunlight color switch is not on by default");
Require(!Looks.Knobs.Any(k => k.Key.Contains("steady", StringComparison.OrdinalIgnoreCase) || k.Key == steady.Env),
    "A look knob uses the steady sunlight color switch");
Require(!Choices.Effects.Any(e => e.Key.Contains("steady", StringComparison.OrdinalIgnoreCase) || e.Key == steady.Env),
    "An effect row uses the steady sunlight color switch");

var ini = new IniFile();
ini.Set("graphics_bloom", "2");
ini.Set("upscaler", "dlss");
Require(!Looks.IsCurrent(Looks.Presets[0], ini), "The original look does not read as current");
var mine = Looks.FromCurrent(ini, "Now", "");
Require(mine.Values.Count == 1 && mine.Values["graphics_bloom"] == "2", "FromCurrent took the wrong values");
Require(Looks.IsCurrent(mine, ini), "A look of the current values reads as current");

Console.WriteLine("PASS looks: " + Looks.Knobs.Length + " knobs, " + Looks.Presets.Count + " presets");
