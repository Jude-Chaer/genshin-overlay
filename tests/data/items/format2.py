"""Item files format 2, as the builder writes them.

Writes what the builder (src/item_files.cpp, write and pack) writes, for cutting the test pack.
Not the code under test: the catalog tests read what this writes and compare it with answers
written by hand.

Format 2 folder:
  tree.json            [{id, name, children: [{id, name}]}]   one, merged from every map's tree
  areas.json           [{id, name, map_id, l_x, l_y, r_x, r_y}] one, merged from every map
  positions/<type>.json [{id, map_id, x_pos, y_pos, area_id, point_group: {group_id, floor_id} | null,
                         appsample: <id> (only when appsample has the point)}]
  icons/<type>.png     as format 1 (at most 64 px)
No appsample.json: each point carries its pair (measured 2026-10-25: load peak +10 MB instead of
+17 MB, and the pack 7.32 MB instead of 7.46 MB).
All json compact (no spaces). HoYoLAB's own key names, so the builder only drops keys.
"""
import json, os

POINT_KEYS = ("id", "map_id", "x_pos", "y_pos", "area_id", "point_group")
AREA_KEYS = ("id", "name", "map_id", "l_x", "l_y", "r_x", "r_y")


def dump(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def trim_point(p, pairs):
    out = {k: p[k] for k in POINT_KEYS}
    g = p.get("point_group")
    out["point_group"] = {"group_id": g["group_id"], "floor_id": g["floor_id"]} if g else None
    if str(p["id"]) in pairs:
        out["appsample"] = pairs[str(p["id"])]
    return out


def merged_tree(src, keep_types=None):
    """Groups and types once, in the order of the first map that has them."""
    groups, order = {}, []
    for m in sorted(os.listdir(os.path.join(src, "maps")), key=int):
        for g in load(os.path.join(src, "maps", m, "tree.json")):
            if g["id"] not in groups:
                groups[g["id"]] = {"id": g["id"], "name": g["name"], "children": []}
                order.append(g["id"])
            have = {c["id"] for c in groups[g["id"]]["children"]}
            for c in g["children"]:
                if c["id"] not in have and (keep_types is None or c["id"] in keep_types):
                    groups[g["id"]]["children"].append({"id": c["id"], "name": c["name"]})
    return [groups[i] for i in order if groups[i]["children"] or keep_types is None]


def merged_areas(src):
    seen, out = set(), []
    for m in sorted(os.listdir(os.path.join(src, "maps")), key=int):
        for a in load(os.path.join(src, "maps", m, "areas.json")):
            if a["id"] not in seen:
                seen.add(a["id"])
                out.append({k: a[k] for k in AREA_KEYS})
    return out


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def pack_folder(folder):
    """Same as ItemFiles::pack: sorted, 'path\\tsize\\n' then the bytes."""
    files = []
    for root, _, names in os.walk(folder):
        for n in names:
            files.append(os.path.relpath(os.path.join(root, n), folder).replace(os.sep, "/"))
    out = bytearray()
    for rel in sorted(files):
        data = open(os.path.join(folder, rel), "rb").read()
        out += f"{rel}\t{len(data)}\n".encode("utf-8") + data
    return bytes(out)
