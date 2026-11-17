using BloodborneLauncher.Core;

static void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}

// HdrGrading.Active is the engine's startup_nativeplus (bbport_settings.cpp: startup_hdr and the
// Native+ source), which is what hdr_grading tests in engine/graphics.cpp: with HDR output on and
// the Native+ source, the chain grades the picture and the port's own brightness, contrast,
// saturation and bloom are left as the game draws them.
Require(HdrGrading.Active("1", "nativeplus"), "Native+ with HDR on must grade the picture");
Require(HdrGrading.Active("1", "psychov30"), "psychov30 is Native+'s name before 0.9.38.2");
Require(!HdrGrading.Active("0", "nativeplus"), "HDR off keeps the port's Picture values in force");
Require(!HdrGrading.Active("", "nativeplus"), "A missing hdr key keeps them in force");
Require(!HdrGrading.Active("1", "native"), "Native is not graded by the Native+ chain");
Require(!HdrGrading.Active("1", "expand"), "Expanded is not graded by the Native+ chain");
Require(!HdrGrading.Active("1", ""), "A missing source keeps them in force");

// The stored hdr_mode as the engine reads it, which the Display page shows and this decides on.
Require(HdrGrading.ModeValue("psychov30") == "nativeplus", "psychov30 must read as nativeplus");
Require(HdrGrading.ModeValue("nativeplus") == "nativeplus", "nativeplus must stay nativeplus");
Require(HdrGrading.ModeValue("native") == "native", "native must stay native");
Require(HdrGrading.ModeValue("expand") == "expand", "expand must stay expand");
Require(HdrGrading.ModeValue("") == "", "An absent source stays absent");

Console.WriteLine("HDR grading checks passed");
