import base64
import contextlib
import io
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import configure


class ConfigurationChecks(unittest.TestCase):
    def test_matches_native_crypto_vector(self):
        self.assertEqual(configure.room_id("hunters", "blood"),
                         "4344e16951b4fdc22c6d41ddc47bd7e6dd67aa9fe320abe75e253f081bd62f56")

    def test_invite_moves_to_our_server_without_changing_room_credentials(self):
        old = "LANTERN-ROOM:" + base64.urlsafe_b64encode(b"room\nsecret\nhttps://old.example.com:8443").decode().rstrip("=")
        name, password = configure.decode_invite(old)
        new = configure.make_invite(name, password, "coop.example.test")
        self.assertEqual(configure.decode_invite(new), ("room", "secret"))
        body = new.split(":", 1)[1]
        decoded = base64.urlsafe_b64decode(body + "=" * (-len(body) % 4)).decode().split("\n")
        self.assertEqual(decoded[2], "https://coop.example.test")

    def test_rejects_invalid_invites(self):
        for code in ("", "LANTERN-VISIT:abc", "LANTERN-ROOM:@@", "LANTERN-ROOM:" + base64.urlsafe_b64encode(b"room\n").decode().rstrip("=")):
            with self.subTest(code=code), self.assertRaises(ValueError):
                configure.decode_invite(code)

    def test_rejects_invalid_domain_configuration(self):
        self.assertEqual(configure.domain_name("COOP.Example.Test"), "coop.example.test")
        for value in ("https://x.example.com", "1.2.3.4", "x.example:443", "x.example\nBAD=1", "${ROOM_ID}", "-x.example"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                configure.domain_name(value)

    def test_configuration_reuses_invite_and_keeps_password_out_of_environment(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "invite.txt").write_text(configure.make_invite("room", "secret-password", "old.example.com"), encoding="utf-8")
            original = "# My settings\nCOMPOSE_PROJECT_NAME=my-coop\nLOG_MAX_SIZE=20m\nLOG_MAX_FILES=5\nROOM_ID=previous\n"
            (root / ".env").write_text(original, encoding="utf-8")
            with patch.object(configure, "__file__", str(root / "configure.py")), \
                    patch.dict(os.environ, {"RELAY_DOMAIN": "coop.example.test"}), \
                    patch("getpass.getpass", side_effect=AssertionError("Reuse the invitation")), \
                    contextlib.redirect_stdout(io.StringIO()):
                configure.main()
            env = (root / ".env").read_text()
            self.assertNotIn("secret-password", env)
            self.assertIn("# My settings\nCOMPOSE_PROJECT_NAME=my-coop\nLOG_MAX_SIZE=20m\nLOG_MAX_FILES=5\n", env)
            self.assertIn("RELAY_DOMAIN=coop.example.test\n", env)
            self.assertIn("ROOM_ID=" + configure.room_id("room", "secret-password") + "\n", env)
            self.assertEqual(env.count("ROOM_ID="), 1)
            self.assertEqual(configure.decode_invite((root / "invite.txt").read_text()), ("room", "secret-password"))

    def test_first_run_prompts_for_host_invite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            code = configure.make_invite("room", "secret-password", "old.example.com")
            with patch.object(configure, "__file__", str(root / "configure.py")), \
                    patch.dict(os.environ, {"RELAY_DOMAIN": "coop.example.test"}), \
                    patch("getpass.getpass", return_value=code) as prompt, \
                    contextlib.redirect_stdout(io.StringIO()):
                configure.main()
            prompt.assert_called_once()
            self.assertTrue((root / ".env").exists())
            self.assertTrue((root / "invite.txt").exists())

    def test_example_domain_requires_configuration_before_prompting(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with patch.object(configure, "__file__", str(root / "configure.py")), \
                    patch.dict(os.environ, {"RELAY_DOMAIN": "coop.example.com"}), \
                    patch("getpass.getpass", side_effect=AssertionError("Validate the host before asking for an invitation")), \
                    self.assertRaisesRegex(ValueError, "Replace RELAY_DOMAIN"):
                configure.main()
            self.assertFalse((root / "invite.txt").exists())

    def test_environment_update_preserves_values_and_replaces_duplicate_generated_keys(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / ".env"
            path.write_text("# My server\nLOG_MAX_FILES=7\nexport ROOM_ID=old\nROOM_ID=duplicate\n", encoding="utf-8")
            configure.update_environment(path, {"ROOM_ID": "new"})
            self.assertEqual(path.read_text(), "# My server\nLOG_MAX_FILES=7\nROOM_ID=new\n")


if __name__ == "__main__":
    unittest.main()
