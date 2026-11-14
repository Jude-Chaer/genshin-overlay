# What the map index and the item files are built from, as one hash per part,
# so the hourly run only builds the part whose sources changed. Each hash
# covers only the fields the builders read (src/map_data.cpp makePlan,
# src/item_files.cpp write and trimPoint, src/appsample.cpp decode).
#
#   python map_sources.py fetch <out file>
#   python map_sources.py compare <current> <published>
#   python map_sources.py merge <published> <current> <out file> [map] [items]
#
# fetch exits 1 when a source didn't answer. compare prints map=true/false and
# items=true/false for $GITHUB_OUTPUT; a missing published file counts as all
# changed. merge writes the published file with the named parts taken from
# current, so a part that failed to build is tried again next hour.

import datetime
import gzip
import hashlib
import json
import pathlib
import sys
import time
import urllib.request

FORMAT = 1
API = "https://sg-public-api.hoyolab.com/common/map_user/ys_obc"
STATIC_API = "https://sg-public-api-static.hoyolab.com/common/map_user/ys_obc"
APPSAMPLE = "https://game-data.lemonapi.com/gim/markers_all.v5.compressed.json"
HEADERS = {"Origin": "https://act.hoyolab.com", "Referer": "https://act.hoyolab.com/", "User-Agent": "genshin-overlay"}
PARTS = ["map", "items", "appsample"]


def fail(text):
    print("map sources: " + text)
    sys.exit(1)


def download(url):
    for attempt in range(3):
        try:
            request = urllib.request.Request(url, headers={**HEADERS, "Accept-Encoding": "gzip"})
            with urllib.request.urlopen(request, timeout=120) as response:
                body = response.read()
                return gzip.decompress(body) if response.headers.get("Content-Encoding") == "gzip" else body
        except OSError as error:
            if attempt == 2:
                fail(url + " didn't answer: " + str(error))
            time.sleep(10)


def hoyolab(path, map_id, static=False):
    body = json.loads(download((STATIC_API if static else API) + path + "?map_id=" + str(map_id) + "&app_sn=ys_obc&lang=en-us"))
    if not isinstance(body, dict) or body.get("retcode") != 0 or "data" not in body:
        fail(path + " of map " + str(map_id) + " answered " + str(body.get("message") if isinstance(body, dict) else body)[:200])
    return body["data"]


# the game's maps, the ones under Teyvat's node (src/map_data.cpp mapIds)
def map_ids():
    body = json.loads(download(STATIC_API + "/v1/map/list?app_sn=ys_obc&lang=en-us"))
    if not isinstance(body, dict) or body.get("retcode") != 0 or "data" not in body:
        fail("the map list answered " + str(body.get("message") if isinstance(body, dict) else body)[:200])
    ids = [item.get("id") for item in body["data"].get("all_map_list") or [] if item.get("node_type") == 2 and item.get("parent_id") == 1]
    if not ids:
        fail("the map list has no maps")
    return ids


def pick(item, keys):
    return {key: item.get(key) for key in keys} if isinstance(item, dict) else None


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def map_part(map_id):
    detail = hoyolab("/v1/map/info", map_id)["info"].get("detail_v2")
    floors = []
    for group in hoyolab("/v2/map/point_group", map_id).get("list", []):
        for floor in group.get("floors") or []:
            overlay = floor.get("overlay")
            if isinstance(overlay, dict) and overlay.get("url"):
                floors.append([group.get("id"), floor.get("id"), pick(overlay, ["url", "l_x", "l_y", "r_x", "r_y"])])
    return [pick(detail, ["total_size", "origin", "map_version"]), sorted(floors, key=json.dumps)]


def icons(nodes, out):
    for node in nodes or []:
        if isinstance(node, dict):
            if node.get("icon"):
                out.append([node.get("id"), node["icon"]])
            icons(node.get("children"), out)


def items_part(map_id):
    tree = hoyolab("/v2/map/label/tree", map_id, static=True)["tree"]
    groups = [[group.get("id"), group.get("name"), [pick(child, ["id", "name"]) for child in group.get("children") or []]]
              for group in tree if isinstance(group, dict)]
    urls = []
    icons(tree, urls)
    areas = [pick(area, ["id", "name", "map_id", "l_x", "l_y", "r_x", "r_y"])
             for area in hoyolab("/v1/map/get_area_pageLabel", map_id, static=True)["list"]]
    points = []
    for point in hoyolab("/v1/map/point/list", map_id)["point_list"]:
        group = point.get("point_group")
        points.append([point.get("id"), point.get("label_id"), point.get("x_pos"), point.get("y_pos"), point.get("area_id"),
                       pick(group, ["group_id", "floor_id"]) if group else None])
    return [groups, sorted(urls, key=json.dumps), areas, sorted(points, key=lambda p: p[0] or 0)]


def appsample_part():
    file = json.loads(download(APPSAMPLE))
    if not isinstance(file, dict) or file.get("code", 0) != 0 or "headers" not in file or "data" not in file:
        fail("appsample's markers file isn't the shape the builder reads")
    columns = ["id", "type", "mid", "lng", "lat", "meta"]
    data = file["data"]
    if file.get("compressionVersion") == 1:
        return [data.get(name) for name in columns]
    at = [file["headers"].index(name) if name in file["headers"] else None for name in columns]
    return [[row[i] if i is not None and i < len(row) else None for i in at] for row in data]


def read(path):
    path = pathlib.Path(path)
    if not path.exists():
        return None
    data = json.loads(path.read_text(encoding="utf-8"))
    return data if isinstance(data, dict) and data.get("format") == FORMAT else None


def write(path, hashes):
    out = {"format": FORMAT, "checked": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), **hashes}
    pathlib.Path(path).write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8", newline="\n")


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else ""
    if command == "fetch" and len(sys.argv) == 3:
        maps = map_ids()
        hashes = {
            "map": digest([map_part(map_id) for map_id in maps]),
            "items": digest([items_part(map_id) for map_id in maps]),
            "appsample": digest(appsample_part()),
        }
        write(sys.argv[2], hashes)
        print("map sources: " + ", ".join(part + " " + hashes[part][:12] for part in PARTS))
    elif command == "compare" and len(sys.argv) == 4:
        current, published = read(sys.argv[2]), read(sys.argv[3])
        if current is None:
            fail(sys.argv[2] + " isn't a sources file")
        changed = [part for part in PARTS if published is None or published.get(part) != current[part]]
        print("map=" + str("map" in changed).lower())
        print("items=" + str("items" in changed or "appsample" in changed).lower())
        print("map sources: changed " + (", ".join(changed) if changed else "nothing"), file=sys.stderr)
    elif command == "merge" and len(sys.argv) >= 5:
        published, current = read(sys.argv[2]) or {}, read(sys.argv[3])
        if current is None:
            fail(sys.argv[3] + " isn't a sources file")
        done = set(sys.argv[5:])
        taken = {"map": "map" in done, "items": "items" in done, "appsample": "items" in done}
        write(sys.argv[4], {part: current[part] if taken[part] else published.get(part) for part in PARTS})
    else:
        fail("usage: map_sources.py fetch <out> | compare <current> <published> | merge <published> <current> <out> [map] [items]")


main()
