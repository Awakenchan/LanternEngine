# SPDX-License-Identifier: GPL-2.0-or-later
import os
import shutil
import sys
import tempfile
import types
from pathlib import Path

launcher_dir = Path(sys.argv[1])


class Anything:
    def __init__(self, *args, **kwargs):
        pass

    def __getattr__(self, name):
        return Anything


repository = types.ModuleType("gi.repository")
for name in ("Adw", "Gio", "GLib", "Gtk"):
    module = types.ModuleType(name)
    module.__getattr__ = lambda attribute: Anything
    setattr(repository, name, module)
gi = types.ModuleType("gi")
gi.require_version = lambda *args: None
gi.repository = repository
sys.modules["gi"] = gi
sys.modules["gi.repository"] = repository
sys.path.insert(0, str(launcher_dir))

folder = Path(tempfile.mkdtemp(prefix="py-ini-check-"))
ini = folder / "bbport.ini"
os.environ["BB_CONFIG"] = str(ini)
import bbport_launcher as launcher  # noqa: E402

values, lines, state = launcher.load_ini()
assert state == launcher.TEXT_ABSENT and lines == [], state
values["upscaler"] = "fsr3"
launcher.save_ini(values)
text = ini.read_bytes()
assert text.startswith(b"# bbport settings") and b"\nupscaler=fsr3\n" in text and b"\r" not in text

ini.write_text("# mine\nunknown=7\nupscaler=fsr3\n", encoding="utf-8")
values, lines, state = launcher.load_ini()
assert state == launcher.TEXT_READ
values["preset"] = "2"
launcher.save_ini(values)
after = ini.read_text(encoding="utf-8")
assert after.startswith("# mine\nunknown=7\nupscaler=fsr3\n") and "\npreset=2\n" in after, after
assert (folder / "bbport.ini.bak").read_text(encoding="utf-8") == "# mine\nunknown=7\nupscaler=fsr3\n"

original = ini.read_bytes()
real_read_text = Path.read_text


def refuse(self, *args, **kwargs):
    if self == ini:
        raise PermissionError(13, "held", str(self))
    return real_read_text(self, *args, **kwargs)


Path.read_text = refuse
try:
    values, lines, state = launcher.load_ini()
    assert state == launcher.TEXT_UNREADABLE and lines == []
    try:
        launcher.save_ini(values)
        raise AssertionError("an unreadable file must not be written")
    except launcher.SettingsUnreadable:
        pass
finally:
    Path.read_text = real_read_text
assert ini.read_bytes() == original
assert not list(folder.glob("*.tmp"))
try:
    launcher.save_ini(dict(values, sharpen="1"))
    raise AssertionError("a save after an unreadable load must be refused even once the file is readable")
except launcher.SettingsUnreadable:
    pass
assert ini.read_bytes() == original
values, lines, state = launcher.load_ini()
assert state == launcher.TEXT_READ and values["preset"] == "2"

real_replace = os.replace


def fail_replace(source, target):
    raise PermissionError(5, "busy", str(target))


os.replace = fail_replace
try:
    launcher.save_ini(dict(values, preset="3"))
    raise AssertionError("a replace that keeps failing must raise")
except PermissionError:
    pass
finally:
    os.replace = real_replace
assert ini.read_bytes() == original
assert not list(folder.glob("*.tmp")), list(folder.glob("*"))
import json  # noqa: E402

mods = folder / "mods.json"
mods.write_text("{ broken", encoding="utf-8")
state, data = launcher.load_json(mods)
assert state == launcher.TEXT_ABSENT and data == {}
assert (folder / "mods.json.broken").read_text(encoding="utf-8") == "{ broken"
launcher.update_json(mods, lambda before: {"order": ["a"]})
assert json.loads(mods.read_text(encoding="utf-8")) == {"order": ["a"]}

patches = folder / "patches.json"
patches.write_text('{"enabled": ["x"]}', encoding="utf-8")
held = patches.read_bytes()


def refuse_patches(self, *args, **kwargs):
    if self == patches:
        raise PermissionError(13, "held", str(self))
    return real_read_text(self, *args, **kwargs)


Path.read_text = refuse_patches
try:
    state, data = launcher.load_json(patches)
    assert state == launcher.TEXT_UNREADABLE and data == {}
finally:
    Path.read_text = real_read_text
try:
    launcher.update_json(patches, lambda before: {"enabled": []})
    raise AssertionError("patches.json must not be written after an unreadable load")
except launcher.SettingsUnreadable:
    pass
assert patches.read_bytes() == held
state, data = launcher.load_json(patches)
assert state == launcher.TEXT_READ and data == {"enabled": ["x"]}
launcher.update_json(patches, lambda before: {"enabled": before["enabled"] + ["y"]})
assert json.loads(patches.read_text(encoding="utf-8")) == {"enabled": ["x", "y"]}
assert (folder / "patches.json.bak").read_bytes() == held
assert not list(folder.glob("*.tmp")), list(folder.glob("*"))
shutil.rmtree(folder, ignore_errors=True)
print("python ini checks passed")
