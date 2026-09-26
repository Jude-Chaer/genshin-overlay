#pragma once

// Graphs of the overlay's own CPU and RAM use, and how long the map matching
// takes, shown in the menu. Samples twice a second and keeps the last minute.

namespace Stats {
    // call every frame, samples in the background so the graphs have history
    extern void Update();
    // the graphs, drawn inside the menu
    extern void Draw();
}
