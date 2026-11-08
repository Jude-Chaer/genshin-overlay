# Item files test pack: the answers

Cut from the real item files downloaded 2026-10-17 (HoYoLAB map API and appsample), trimmed to
format 2 by cut_test_pack.py (with format2.py) next to this file. Every answer below was read off the files by
hand, not produced by the overlay's code.

Maps: 2 Teyvat, 7 Enkanomiya, 9 The Chasm: Underground Mines, 34 Sea of Bygone Eras.

## The merged type list (GetItemTypes)

In this order, groups and types once each (HoYoLAB's tree order):

| Group | Type | Points per map |
|---|---|---|
| 1 Waypoints | 589 Temple of Silence | none (no positions file) |
| 186 Puzzle Chest | 509 Fontaine Shrine of Depths | 2: 8, 34: 2 |
| 186 Puzzle Chest | 231 Light-Up Tile Puzzle | 2: 5, 9: 1 |
| 51 Experience | 189 Treasure Map | 2: 11 |
| 232 Fishing | 239 Lunged Stickleback | 2: 7, 7: 1 |

Total points: 10 + 6 + 11 + 8 = 35.

## Points of one type, split by map (GetItemPositions(231))

| id | map | x | y | area | group | floor |
|---|---|---|---|---|---|---|
| 47783 | 2 | 3008.75 | 6754.75 | 3 | 84 | 9 |
| 17631 | 2 | 3498 | 6556.5 | 3 | 0 | 0 |
| 17630 | 2 | 3530.5 | 7074.5 | 3 | 0 | 0 |
| 17629 | 2 | 3275 | 6796 | 3 | 0 | 0 |
| 17628 | 2 | 2998 | 6608.5 | 3 | 0 | 0 |
| 33158 | 9 | 239 | 426.5 | 6 | 0 | 0 |

## Underground

Type 189 Treasure Map: 8 of its 11 points, on two different floors:
- group 90 floor 78: 56249, 56247, 56246, 56245, 56244, 56243, 56242 (7 points)
- group 109 floor 133: 56248 (1 point)
- surface: 82140, 82139, 82138

Type 509 Fontaine Shrine of Depths: 54044 and 50961 on group 90 floor 78, the other 8 on the
surface. So group 90 floor 78 holds points of two types: 7 + 2 = 9.

## A point by id (the catalog's point(id), what HoyolabMarks asks)

- 23275: map 7, type 239, x -1178.8999938964844, y 776.5053532876846, area 5
- 56248: map 2, type 189, group 109, floor 133
- 68808: map 34, type 509, x 75.5, y 147.5, area 9

## Regions (GetItemRegions)

14 regions, once each (every map's areas.json holds all 14). Two boxes:
- 1 Mondstadt, map 2: left -1031, top -688, right 1195, bottom 1069
- 6 The Chasm: Underground Mines, map 9: left -659, top -436, right 505, bottom 433

Trap: a point's region is its `area_id`, not the box it sits in. The Inazuma puzzle points (area 3)
are at y 6556 to 7074, outside Inazuma's box (y 4550 to 6286).

## Appsample pairs

- 47783 -> 48401
- 33158 -> 42251
- 23275 -> 25865
- every point of 509 Fontaine Shrine of Depths -> none (appsample doesn't have that type)

### Close points, checked by eye (tests/data/items/appsample)

Maps 37 and 40 cut from the item files of 2026-10-25 and appsample's markers of 2026-10-26 by
`cut.py` there (1837 and 1774 points). Each pair below was read off both maps: appsample's point moved
by the shift of the pairs around it (within 300 px), then the one that lands on ours, 0.02 to 0.2 px
off, while the other of the two or three is 2 to 4 px away.

| Map | Type | HoYoLAB -> appsample |
|---|---|---|
| 40 | 682 Ancient Moon Pearl (group 206 floor 300) | 100287 -> 100942, 100288 -> 100941, 100289 -> 100940, 100290 -> 100939, 100291 -> 100938, 100292 -> 100937 |
| 37 | 799 | 96839 -> 97443, 96840 -> 97442, 96841 -> 97441 |
| 37 | 175 | 99146 -> 99749, 99147 -> 99748 |
| 37 | 122 | 97335 -> 98125, 97336 -> 98124 |
| 37 | 177 | 99185 -> 99710, 99186 -> 99709 |
| 37 | 180 | 99208 -> 99687, 99209 -> 99686 |

Trap: with one offset per region these pairs cross, appsample's map is a pixel or two off inside a
region, as much as the two points are apart.

## Per map (items.json's "maps", what the builder checks)

Counted off the positions files: points, types with a point there, points with an appsample pair.

| Map | Points | Types | Pairs |
|---|---|---|---|
| 2 | 31 | 4 | 23 |
| 7 | 1 | 1 | 1 |
| 9 | 1 | 1 | 1 |
| 34 | 2 | 1 | 0 |

## Icons

- 231 and 239 have one.
- 189 and 509 have none in this pack, and 589 has none anywhere: GetItemIcon gives nil.

## Unknown ids (nil, never an error)

- type 999999: GetItemPositions and GetItemIcon give nil
- point 1: no point, no pair

## Packs

- items.pack.gz with items.json (format 2): unpacks to exactly folder/ (8 files: tree.json,
  areas.json, 4 positions, 2 icons), sha256 matches.
- bad-dotdot.pack (`../escaped.txt`), bad-absolute.pack (`C:/escaped.txt`), bad-size.pack (size
  50, 2 bytes left): all refused. Each starts with a good `tree.json`, so a test can check the old
  items folder is untouched and nothing was written outside.
