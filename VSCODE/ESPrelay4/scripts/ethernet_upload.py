"""Authenticated HTTP OTA with a durable snapshot taken before any upload."""

import base64
import hashlib
import http.client
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path
from urllib.parse import urlsplit

CONFIG_NAMES = ("rules", "net", "mqtt", "wifi", "ble", "auth")


def get_json(url, token, endpoint):
    connection = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=5)
    try:
        connection.request("GET", endpoint, headers={"Authorization": "Basic " + token})
        response = connection.getresponse()
        if response.status != 200:
            raise ValueError("{} : HTTP {}".format(endpoint, response.status))
        result = json.loads(response.read())
        if not isinstance(result, dict):
            raise ValueError("{} : réponse invalide".format(endpoint))
        return result
    finally:
        connection.close()


def read_settings(url, token):
    settings = get_json(url, token, "/api/backup")
    for name in CONFIG_NAMES[:-1]:
        if not isinstance(settings.get(name), dict):
            raise ValueError("sauvegarde incomplète : {} ; aucun upload effectué".format(name))
    if settings.get("schema", 2) != 2:
        raise ValueError("version de sauvegarde non prise en charge")
    return settings


def save_snapshot(env, url, token, user, password, firmware_target=None):
    # Read settings first, before firmware migrations can change their contents.
    settings = read_settings(url, token)
    state = get_json(url, token, "/api/state")
    if not state.get("device_id") or not state.get("fw"):
        raise ValueError("identité/version de la carte indisponible")
    if "auth" not in settings:  # Legacy firmware omitted authenticated credentials.
        settings["auth"] = {"user": user, "pass": password}
    if not isinstance(settings["auth"], dict) or not settings["auth"].get("user") or not settings["auth"].get("pass"):
        raise ValueError("identifiants de sauvegarde incomplets")
    settings["schema"] = 2
    directory = Path(env.subst("$PROJECT_DIR")) / ".pio" / "ota-backups"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    os.chmod(directory, 0o700)
    path = directory / (time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex + ".json")
    snapshot = {"destination": url.netloc, "device_id": state["device_id"],
                "firmware_before": state["fw"], "settings": settings}
    if firmware_target:
        snapshot["firmware_target"] = firmware_target
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as stream:
        json.dump(snapshot, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    env["ESPRELAY_OTA_SNAPSHOT"] = str(path.resolve())
    print("OTA: sauvegarde avant upload enregistrée dans {}".format(path))
    return snapshot


def load_snapshot(env, url):
    path = os.environ.get("ESPRELAY_OTA_SNAPSHOT") or env.get("ESPRELAY_OTA_SNAPSHOT")
    if not path:
        return None
    snapshot = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(snapshot, dict):
        raise ValueError("fichier de sauvegarde invalide")
    if snapshot.get("destination") != url.netloc or not snapshot.get("device_id"):
        raise ValueError("sauvegarde destinée à une autre carte")
    settings = snapshot.get("settings", {})
    if settings.get("schema") != 2 or any(not isinstance(settings.get(n), dict) for n in CONFIG_NAMES):
        raise ValueError("fichier de sauvegarde incomplet")
    return snapshot


def filesystem_payload(image, env, settings):
    tool_dir = env.PioPlatform().get_package_dir("tool-mklittlefs")
    tool = Path(tool_dir) / ("mklittlefs.exe" if os.name == "nt" else "mklittlefs")
    with tempfile.TemporaryDirectory(prefix="esprelay-ota-") as directory:
        staged = Path(directory) / "data"
        shutil.copytree(env.subst("$PROJECT_DATA_DIR"), staged)
        files = {}
        for name in CONFIG_NAMES:
            files["/" + name + ".json"] = settings[name]
            (staged / (name + ".json")).write_text(json.dumps(settings[name]), encoding="utf-8")
        # New firmware reads this canonical atomic store; old versions read the six legacy files.
        (staged / "settings.json").write_text(json.dumps({"schema": 1, "files": files}), encoding="utf-8")
        (staged / "settings.json.tmp").unlink(missing_ok=True)
        output = Path(directory) / "littlefs.bin"
        subprocess.run([str(tool), "-c", str(staged), "-s", str(image.stat().st_size),
                        "-p", "256", "-b", "4096", str(output)], check=True)
        print("OTA: image web construite avec les réglages sauvegardés avant le firmware.")
        return output.read_bytes()


def settings_match(expected, actual):
    # Firmware exports may include derived connection/IP fields; compare all saved settings only.
    for name in CONFIG_NAMES:
        actual_section = actual.get(name)
        if name == "auth" and actual_section is None:
            continue  # Legacy firmware: HTTP authentication verifies the restored account.
        if not isinstance(actual_section, dict):
            return False
        if any(actual_section.get(k) != v for k, v in expected[name].items()
               if k not in ("_revision", "client_id", "device_id", "base_effective")):
            return False
    return True


def verify_reboot(env, url, token, snapshot, verify_settings=False, before_boot=None):
    deadline = time.monotonic() + 60
    time.sleep(2)
    last_error = "carte indisponible"
    while time.monotonic() < deadline:
        try:
            state = get_json(url, token, "/api/state")
            if state.get("device_id") != snapshot["device_id"]:
                raise ValueError("l'identité de la carte a changé")
            expected_fw = snapshot.get("firmware_target", snapshot["firmware_before"])
            if expected_fw and state.get("fw") != expected_fw:
                raise ValueError("la version attendue du firmware n'est pas active")
            if not isinstance(state.get("uptime_ms"), int) or not 0 <= state["uptime_ms"] <= 65000:
                raise ValueError("redémarrage non confirmé")
            if before_boot is not None:
                before_state, sampled_at = before_boot
                elapsed_ms = (time.monotonic() - sampled_at) * 1000
                # The estimated boot time must advance, even when flashing the same version.
                if state["uptime_ms"] + 500 >= before_state["uptime_ms"] + elapsed_ms:
                    raise ValueError("le compteur de fonctionnement ne confirme pas le redémarrage")
            if verify_settings and not settings_match(snapshot["settings"], read_settings(url, token)):
                raise ValueError("les réglages restaurés diffèrent de la sauvegarde")
            print("OTA: identité, version et {} vérifiés après redémarrage.".format(
                "réglages" if verify_settings else "état"))
            return True
        except (OSError, ValueError, http.client.HTTPException) as error:
            last_error = str(error)
            time.sleep(2)
    print("OTA: vérification échouée : {}. Sauvegarde conservée.".format(last_error))
    return False


def upload_web_after_firmware(source, target, env):
    if env.GetProjectOption("custom_upload_web", "yes").lower() not in ("yes", "true", "1"):
        return 0
    # Never launch uploadfs after a failed firmware action.
    if not env.get("ESPRELAY_OTA_FIRMWARE_OK") or not env.get("ESPRELAY_OTA_SNAPSHOT"):
        return 1
    child_env = os.environ.copy()
    child_env["ESPRELAY_OTA_SNAPSHOT"] = env["ESPRELAY_OTA_SNAPSHOT"]
    return subprocess.call([sys.executable, "-m", "platformio", "run", "-e", env.subst("$PIOENV"),
                            "-t", "uploadfs", "--upload-port", str(env["UPLOAD_PORT"])],
                           cwd=env.subst("$PROJECT_DIR"), env=child_env)


def upload_firmware(source, target, env):
    env["ESPRELAY_OTA_FIRMWARE_OK"] = False
    image = Path(str(source[0]))
    is_fs = image.name == "littlefs.bin"
    endpoint = "/api/otafs" if is_fs else "/api/ota"
    label = "LittleFS" if is_fs else "firmware"
    destination = str(env.get("UPLOAD_PORT") or "").strip()
    user = os.environ.get("ESPRELAY_OTA_USER", env.GetProjectOption("custom_ota_user", "admin"))
    password = os.environ.get("ESPRELAY_OTA_PASSWORD", env.GetProjectOption("custom_ota_password", ""))
    if not destination or not password:
        print("OTA: renseignez upload_port et custom_ota_password dans platformio.local.ini.")
        return 1
    url = urlsplit(destination if "://" in destination else "http://" + destination)
    if url.scheme != "http" or not url.hostname or url.username or url.password or url.path not in ("", "/") or url.query or url.fragment:
        print("OTA: upload_port doit être une adresse IP ou un nom DNS, avec port HTTP optionnel.")
        return 1
    connection = None
    try:
        token = base64.b64encode((user + ":" + password).encode()).decode()
        snapshot = load_snapshot(env, url) if is_fs else None
        if snapshot is None:
            snapshot = save_snapshot(env, url, token, user, password,
                                     None if is_fs else env.get("ESPRELAY_FW_VERSION"))
        elif get_json(url, token, "/api/state").get("device_id") != snapshot["device_id"]:
            raise ValueError("la carte ne correspond pas à la sauvegarde")
        payload = filesystem_payload(image, env, snapshot["settings"]) if is_fs else image.read_bytes()
        if not payload:
            raise ValueError("fichier {} vide".format(label))
        before_state = get_json(url, token, "/api/state")
        if before_state.get("device_id") != snapshot["device_id"] or not isinstance(before_state.get("uptime_ms"), int):
            raise ValueError("identité/état de la carte incohérent avant upload")
        if before_state["uptime_ms"] < 1000:
            time.sleep(1)  # Give a newly booted board enough uptime to prove the next reboot.
            before_state = get_json(url, token, "/api/state")
            if before_state.get("device_id") != snapshot["device_id"] or not isinstance(before_state.get("uptime_ms"), int) or before_state["uptime_ms"] < 1000:
                raise ValueError("compteur de fonctionnement indisponible avant upload")
        before_boot = (before_state, time.monotonic())
        connection = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=120)
        print("OTA: envoi de {} octets vers {}...".format(len(payload), destination))
        connection.request("POST", endpoint, body=payload, headers={
            "Content-Type": "application/octet-stream",
            "Authorization": "Basic " + token,
            "X-Checksum-Sha256": hashlib.sha256(payload).hexdigest(),
        })
        response = connection.getresponse()
        body = response.read().decode("utf-8", errors="replace")
        if response.status != 200:
            print("OTA: échec HTTP {} ; sauvegarde conservée.".format(response.status))
            return 1
        if json.loads(body).get("ok") is not True:
            raise ValueError("la carte n'a pas confirmé la mise à jour")
        connection.close()
        connection = None
        if not verify_reboot(env, url, token, snapshot, verify_settings=is_fs, before_boot=before_boot):
            return 1
        if not is_fs:
            env["ESPRELAY_OTA_FIRMWARE_OK"] = True
        return 0
    except (OSError, ValueError, http.client.HTTPException, subprocess.CalledProcessError) as error:
        print("OTA: échec de la mise à jour: {}".format(error))
        return 1
    finally:
        if connection is not None:
            connection.close()


try:
    Import("env")
except NameError:
    pass
else:
    destination = str(env.get("UPLOAD_PORT") or "").strip()
    if destination and "://" not in destination:
        env.Replace(UPLOAD_PORT="http://" + destination)
    env.Replace(UPLOADCMD=upload_firmware)
    env.AddPostAction("upload", upload_web_after_firmware)
