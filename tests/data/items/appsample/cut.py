# Cuts maps 37 and 40 out of the real files for the pairing test.
#   python cut.py <items folder with positions\> <appsample markers.json>
# Writes ours.tsv (HoYoLAB's points) and markers.json (appsample's, in their own format,
# ids still as steps) next to this script.
import json, os, sys, glob

MAPS = {37, 40}
folder, markers = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))

rows = []
for path in glob.glob(os.path.join(folder, 'positions', '*.json')):
    kind = int(os.path.basename(path)[:-5])
    for point in json.load(open(path, encoding='utf-8')):
        if point['map_id'] not in MAPS:
            continue
        group = point.get('point_group') or {}
        rows.append((point['id'], point['map_id'], kind, point['area_id'], group.get('group_id', 0), group.get('floor_id', 0), point['x_pos'], point['y_pos']))
with open(os.path.join(here, 'ours.tsv'), 'w', newline='\n') as out:
    out.write('# id\tmap\ttype\tarea\tgroup\tfloor\tx\ty\n')
    for row in sorted(rows):
        out.write('\t'.join(str(v) for v in row) + '\n')

body = json.load(open(markers, encoding='utf-8'))
data = body['data']
columns = body['headers']
kept = {column: [] for column in columns}
last = 0
step = 0
for k in range(len(data['id'])):
    step += data['id'][k]
    if data['mid'][k] not in MAPS:
        continue
    for column in columns:
        kept[column].append(data[column][k])
    kept['id'][-1] = step - last
    last = step
body['data'] = kept
with open(os.path.join(here, 'markers.json'), 'w', encoding='utf-8', newline='\n') as out:
    json.dump(body, out, separators=(',', ':'), ensure_ascii=False)
print(len(rows), 'ours,', len(kept['id']), 'theirs')
