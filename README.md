# genshin-overlay

An interactive map overlay for Genshin Impact. Open the map in game and the markers you choose
show up right on it: chests, oculi, materials, bosses, whatever you are looking for. No alt-tabbing
to a website.

## Features

- **Snaps to the in-game map.** Press M and the overlay finds where the map is looking, in any
  region and on any underground layer.
- **Follows along.** Drag or zoom the map and the markers move with it.
- **Pick what to show.** Press `` ` `` to open the marker menu and choose categories.
- **Transparent and click-through.** The markers look like part of the game's own map.
- **Extensions.** Add your own panels and markers with Lua scripts.

## How it works

The overlay only reads the screen. It never touches the game's memory or files.

1. The game window is captured a few times per second.
2. The frame is matched against the official map art (SIFT features and a RANSAC fit with
   OpenCV). This gives the region, the underground layer, the spot the map is centred on and
   the zoom level.
3. While the map is open it keeps tracking with lighter matches. When nothing matches, the map is
   treated as closed and the markers are hidden.
4. Markers are placed on screen from that position and drawn with ImGui on a transparent window
   over the game.

## Status

Early development. Nothing to download yet.

- [ ] Overlay window and keybinds
- [ ] Game capture
- [ ] Map matching and tracking
- [ ] Markers and the marker menu
- [ ] Lua extensions
- [ ] Boss HP and health

## Building

Windows only.

1. Install Visual Studio 2022 with the "Desktop development with C++" workload. It comes with
   vcpkg.
2. Once, from a Developer PowerShell: `vcpkg integrate install`
3. Open `genshin-overlay.sln` and build `Release | x64`. The first build installs everything in
   `vcpkg.json` by itself. OpenCV takes a while the first time, after that it's quick.
4. Run `build\Release\genshin-overlay.exe` and press `` ` `` to open the menu. On the first start
   it downloads the map data into `map_data` next to the exe (about 26 MB).

To test the map matching without the game, run it on a screenshot of the in-game map:

```
genshin-overlay.exe --locate screenshot.png
```

## Credits

Started by [Mippy](https://github.com/Mipppy), who came up with the idea and built the first
versions ([a_test](https://github.com/Mipppy/a_test), [na](https://github.com/Mipppy/na)).
Map matching by [Jude](https://github.com/Jude-Chaer).

Not affiliated with HoYoverse. Genshin Impact and its map art belong to HoYoverse.
