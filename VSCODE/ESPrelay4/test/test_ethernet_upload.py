"""Offline OTA regressions: snapshot ordering, staging and reboot verification."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import urlsplit

spec = importlib.util.spec_from_file_location(
    "ethernet_upload", Path(__file__).resolve().parents[1] / "scripts/ethernet_upload.py")
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


class Env(dict):
    def GetProjectOption(self, key, default):
        return self.get(key, default)

    def subst(self, value):
        return {"$PROJECT_DATA_DIR": self["data"], "$PROJECT_DIR": self["project"],
                "$PIOENV": "ethernet"}.get(value, value)

    def PioPlatform(self):
        return self

    def get_package_dir(self, name):
        return self["tool"]


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.env = Env(UPLOAD_PORT="192.0.2.1", custom_ota_password="test", ESPRELAY_FW_VERSION="new",
                       data=str(self.root / "data"), project=str(self.root), tool=str(self.root))
        (self.root / "data").mkdir()
        (self.root / "data/index.html").write_text("new interface")
        self.settings = {key: {"value": key} for key in ("rules", "net", "mqtt", "wifi", "ble")}
        self.settings["auth"] = {"user": "admin", "pass": "test"}
        self.settings["schema"] = 2
        self.state = {"device_id": "card", "fw": "old", "uptime_ms": 200000}
        self.url = urlsplit("http://192.0.2.1")
        self.clean_env = patch.dict(os.environ, {k: v for k, v in os.environ.items() if not k.startswith("ESPRELAY_OTA_")}, clear=True)
        self.clean_env.start()
        self.addCleanup(self.clean_env.stop)

    def image(self, name):
        image = self.root / name
        image.write_bytes(b"image")
        return image

    def snapshot(self, target=None):
        with patch.object(ota, "read_settings", return_value=self.settings), \
                patch.object(ota, "get_json", return_value=self.state):
            return ota.save_snapshot(self.env, self.url, "token", "admin", "test", target)

    def test_snapshot_is_durable_private_and_precedes_firmware_post(self):
        with patch.object(ota, "read_settings", return_value=self.settings), \
                patch.object(ota, "get_json", return_value=self.state), \
                patch.object(ota, "verify_reboot", return_value=True), \
                patch.object(ota.http.client, "HTTPConnection") as factory:
            def request(*args, **kwargs):
                saved = Path(self.env["ESPRELAY_OTA_SNAPSHOT"])
                self.assertEqual(json.loads(saved.read_text())["settings"], self.settings)
                if os.name != "nt":
                    self.assertEqual(saved.stat().st_mode & 0o777, 0o600)
                self.assertEqual(args, ("POST", "/api/ota"))
            factory.return_value.request.side_effect = request
            response = factory.return_value.getresponse.return_value
            response.status = 200
            response.read.return_value = b'{"ok":true}'
            self.assertEqual(ota.upload_firmware([self.image("firmware.bin")], [], self.env), 0)
            self.assertTrue(self.env["ESPRELAY_OTA_FIRMWARE_OK"])

    def test_backup_failure_prevents_all_uploads(self):
        with patch.object(ota, "read_settings", side_effect=ValueError("incomplete")), \
                patch.object(ota.http.client, "HTTPConnection") as factory:
            for name in ("firmware.bin", "littlefs.bin"):
                self.assertEqual(ota.upload_firmware([self.image(name)], [], self.env), 1)
            factory.assert_not_called()

    def test_staged_image_uses_pre_firmware_snapshot_and_canonical_store(self):
        self.snapshot("new")
        # Changes produced by the new firmware must not become the preserved settings.
        with patch.object(ota, "read_settings", side_effect=AssertionError("must not re-read changed settings")):
            snapshot = ota.load_snapshot(self.env, self.url)
        def build(args, check):
            staged = Path(args[args.index("-c") + 1])
            files = json.loads((staged / "settings.json").read_text())["files"]
            for name in ota.CONFIG_NAMES:
                actual = json.loads((staged / (name + ".json")).read_text())
                self.assertEqual(actual, self.settings[name])
                self.assertEqual(files["/" + name + ".json"], actual)
            self.assertEqual((staged / "index.html").read_text(), "new interface")
            Path(args[-1]).write_bytes(b"new preserved image")
        with patch.object(ota.subprocess, "run", side_effect=build):
            payload = ota.filesystem_payload(self.image("littlefs.bin"), self.env, snapshot["settings"])
        self.assertEqual(payload, b"new preserved image")
        self.assertEqual([p.name for p in (self.root / "data").iterdir()], ["index.html"])

    def test_no_web_upload_after_failed_firmware_and_snapshot_is_forwarded(self):
        self.snapshot("new")
        with patch.object(ota.subprocess, "call", return_value=0) as call:
            self.assertEqual(ota.upload_web_after_firmware([], [], self.env), 1)
            call.assert_not_called()
            self.env["ESPRELAY_OTA_FIRMWARE_OK"] = True
            self.assertEqual(ota.upload_web_after_firmware([], [], self.env), 0)
            self.assertIn("uploadfs", call.call_args.args[0])
            self.assertEqual(call.call_args.kwargs["env"]["ESPRELAY_OTA_SNAPSHOT"], self.env["ESPRELAY_OTA_SNAPSHOT"])
            self.env["custom_upload_web"] = "no"
            call.reset_mock()
            self.assertEqual(ota.upload_web_after_firmware([], [], self.env), 0)
            call.assert_not_called()

    def test_reboot_verifies_identity_firmware_uptime_and_settings(self):
        snapshot = self.snapshot("new")
        state = dict(self.state, fw="new", uptime_ms=1000)
        with patch.object(ota, "get_json", return_value=state), \
                patch.object(ota, "read_settings", return_value=self.settings), patch.object(ota.time, "sleep"):
            self.assertTrue(ota.verify_reboot(self.env, self.url, "token", snapshot, True))
        for wrong in (dict(state, fw="old"), dict(state, device_id="other"), dict(state, uptime_ms=200000)):
            with patch.object(ota, "get_json", return_value=wrong), patch.object(ota.time, "sleep"), \
                    patch.object(ota.time, "monotonic", side_effect=[0, 1, 61]):
                self.assertFalse(ota.verify_reboot(self.env, self.url, "token", snapshot))
        changed = dict(self.settings, rules={"value": "modified"})
        self.assertFalse(ota.settings_match(self.settings, changed))

    def test_same_version_requires_uptime_reset(self):
        snapshot = self.snapshot("new")
        before = ({"uptime_ms": 10000}, 0)
        for uptime, valid in ((1000, True), (11000, False)):
            with patch.object(ota, "get_json", return_value=dict(self.state, fw="new", uptime_ms=uptime)), \
                    patch.object(ota.time, "sleep"), \
                    patch.object(ota.time, "monotonic", side_effect=[0, 1, 1, 61]):
                self.assertEqual(ota.verify_reboot(self.env, self.url, "token", snapshot, before_boot=before), valid)

    def test_standalone_filesystem_upload_checks_existing_firmware_version(self):
        snapshot = self.snapshot()
        with patch.object(ota, "get_json", return_value=dict(self.state, uptime_ms=500)), \
                patch.object(ota.time, "sleep"):
            self.assertTrue(ota.verify_reboot(self.env, self.url, "token", snapshot))
        with self.assertRaises(ValueError):
            ota.load_snapshot(self.env, urlsplit("http://192.0.2.2"))


if __name__ == "__main__":
    unittest.main()
