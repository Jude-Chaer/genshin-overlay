# Puts Irminsul's dispatch keys (keys/gi.json) and auto-artifactarium's login
# keys (keys/private_key_N.pem) in one traffic-keys.json for the overlay, see
# src/packets/traffic_keys.hpp.
#
#   python traffic_keys.py <folder> <out file>
#
# The folder has gi.json, the private_key_N.pem files, from.json (the commits
# they're from) and published.json when the release has one. A key that's
# published stays in, so a key dropped upstream is never lost to players.
# Exits 3 when the keys are the published ones, 1 when a file isn't right.

import base64
import binascii
import datetime
import json
import pathlib
import re
import sys

FORMAT = 1
KEY_SIZE = 4096
PEM = re.compile(r"-----BEGIN RSA PRIVATE KEY-----\n([A-Za-z0-9+/=\n]+)-----END RSA PRIVATE KEY-----\n?")


def fail(text):
    print("traffic keys: " + text)
    sys.exit(1)


def check_dispatch(keys, where):
    if not isinstance(keys, dict) or not keys:
        fail(where + " has no dispatch keys")
    for key_id, key in keys.items():
        if not key_id.isdigit() or not 0 < int(key_id) <= 65535 or str(int(key_id)) != key_id:
            fail(where + ": dispatch key id " + key_id + " isn't a number the game sends")
        try:
            size = len(base64.b64decode(key, validate=True))
        except (binascii.Error, TypeError):
            fail(where + ": dispatch key " + key_id + " isn't base64")
        if size != KEY_SIZE:
            fail(where + ": dispatch key " + key_id + " is " + str(size) + " bytes, not " + str(KEY_SIZE))


def check_pem(text, where):
    if not PEM.fullmatch(text):
        fail(where + " isn't an RSA private key")
    try:
        base64.b64decode(PEM.fullmatch(text).group(1).replace("\n", ""), validate=True)
    except binascii.Error:
        fail(where + " isn't base64 inside")


def main():
    if len(sys.argv) != 3:
        fail("usage: traffic_keys.py <folder> <out file>")
    folder = pathlib.Path(sys.argv[1])

    dispatch = json.loads((folder / "gi.json").read_text(encoding="utf-8"))
    check_dispatch(dispatch, "gi.json")
    rsa = {}
    for path in sorted(folder.glob("private_key_*.pem")):
        number = path.stem[len("private_key_"):]
        if not number.isdigit():
            fail(path.name + " isn't private_key_<number>.pem")
        text = path.read_text(encoding="utf-8").replace("\r\n", "\n")
        check_pem(text, path.name)
        rsa[str(int(number))] = text
    if not rsa:
        fail("no private_key_N.pem")

    published = None
    if (folder / "published.json").exists():
        published = json.loads((folder / "published.json").read_text(encoding="utf-8"))
        if not isinstance(published, dict) or published.get("format") != FORMAT:
            fail("the published traffic-keys.json isn't format " + str(FORMAT))
        check_dispatch(published.get("dispatch"), "the published file")
        for number, text in published.get("rsa", {}).items():
            check_pem(text, "the published login key " + number)
        # upstream's key wins for the same id
        dispatch = {**published["dispatch"], **dispatch}
        rsa = {**published.get("rsa", {}), **rsa}

    dispatch = dict(sorted(dispatch.items(), key=lambda item: int(item[0])))
    rsa = dict(sorted(rsa.items(), key=lambda item: int(item[0])))
    if published and published["dispatch"] == dispatch and published.get("rsa") == rsa:
        print("traffic keys: the same as published, " + str(len(dispatch)) + " dispatch and " + str(len(rsa)) + " login keys")
        sys.exit(3)

    out = {
        "format": FORMAT,
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "dispatch": dispatch,
        "rsa": rsa,
        "from": json.loads((folder / "from.json").read_text(encoding="utf-8")),
    }
    pathlib.Path(sys.argv[2]).write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8", newline="\n")
    if published:
        added = [key_id for key_id in dispatch if key_id not in published["dispatch"]]
        print("traffic keys: new dispatch keys " + (", ".join(added) if added else "none") + ", login keys " + ", ".join(rsa))
    else:
        print("traffic keys: first file, " + str(len(dispatch)) + " dispatch and " + str(len(rsa)) + " login keys")


main()
