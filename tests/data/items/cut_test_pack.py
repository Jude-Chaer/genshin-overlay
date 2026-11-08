"""Cuts the item files test pack out of the real item files (2026-10-17 download).

python -I cut_test_pack.py <real items folder> <out folder>
<real items folder> is the 2026-10-17 download: maps/<map>/tree.json and areas.json,
positions/, icons/, appsample.json. <out> becomes tests/data/items:
  folder/              format 2 item folder, cut down (what the catalog loads)
  items.pack.gz, items.json   folder/ packed, as published on the release
  bad-dotdot.pack, bad-absolute.pack, bad-size.pack   raw (not gzipped) packs unpack must refuse
The answers are written by hand in answers.md, not by this script.
"""
import gzip, hashlib, json, os, shutil, sys

sys.dont_write_bytecode = True  # no __pycache__ in tests/data

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import format2 as f2

# Light-Up Tile Puzzle, Lunged Stickleback, Treasure Map, Temple of Silence, Fontaine Shrine of Depths
TYPES = (231, 239, 189, 589, 509)
ICONS = (231, 239)              # the others left without one on purpose

src, out = sys.argv[1], sys.argv[2]
folder = os.path.join(out, "folder")
if os.path.exists(folder):  # only what this script makes; answers.md is written by hand
    shutil.rmtree(folder)

f2.write(os.path.join(folder, "tree.json"), f2.dump(f2.merged_tree(src, set(TYPES))))
f2.write(os.path.join(folder, "areas.json"), f2.dump(f2.merged_areas(src)))
pairs = f2.load(os.path.join(src, "appsample.json"))
for t in TYPES:
    path = os.path.join(src, "positions", f"{t}.json")
    if os.path.exists(path):  # 589 has no positions file, like 36 other types
        f2.write(os.path.join(folder, "positions", f"{t}.json"), f2.dump([f2.trim_point(p, pairs) for p in f2.load(path)]))
for t in ICONS:
    os.makedirs(os.path.join(folder, "icons"), exist_ok=True)
    shutil.copyfile(os.path.join(src, "icons", f"{t}.png"), os.path.join(folder, "icons", f"{t}.png"))

raw = f2.pack_folder(folder)
packed = gzip.compress(raw, 9, mtime=0)
open(os.path.join(out, "items.pack.gz"), "wb").write(packed)
f2.write(os.path.join(out, "items.json"), json.dumps({
    "format": 2, "built": "2026-10-17T13:42:00Z",
    "size": len(packed), "rawSize": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}, indent=1))

# a good file first, so a test can check nothing of a refused pack is left behind
good = b'tree.json\t2\n[]'
open(os.path.join(out, "bad-dotdot.pack"), "wb").write(good + b'../escaped.txt\t2\nhi')
open(os.path.join(out, "bad-absolute.pack"), "wb").write(good + b'C:/escaped.txt\t2\nhi')
open(os.path.join(out, "bad-size.pack"), "wb").write(good + b'areas.json\t50\n[]')
print("wrote", out, len(raw), "bytes raw,", len(packed), "packed")
