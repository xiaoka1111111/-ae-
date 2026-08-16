#include "../preset_data.h"
#include <cstdio>
int main() {
    const Preset& p = kPresets[15];  // Fire Wave
    printf("FireWave: nLayers=%d\n", p.nLayers);
    for (int i = 0; i < p.nLayers; i++) {
        const PresetLayer& L = p.layers[i];
        printf("  layer%d order=%d mode=%d grow=%.1f overlay=%d op=%.0f start=%.0f end=%.0f blur=%.0f nStops=%d\n",
               i, L.order, L.mode, L.grow, L.overlayMode, L.overlayOpacity, L.start, L.end, L.blur, L.nStops);
    }
    return 0;
}
