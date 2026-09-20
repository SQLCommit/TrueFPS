// Layout calculations for the ImGui panel.
#pragma once
#include <algorithm>

namespace truefps {
struct PanelSize { float width, height; };
inline PanelSize panelWindowSize(float scale, float viewportWidth, float viewportHeight) {
    return {(std::min)(900.0f * scale, (std::max)(1.0f, viewportWidth - 24.0f)),
            (std::min)(600.0f * scale, (std::max)(1.0f, viewportHeight - 24.0f))};
}
inline bool panelStacked(float available, float sidebar, float scale) {
    return available < sidebar + 360.0f * scale;
}
struct PanelSizing {
    float lastScale = 0.0f;
    bool requested = false;
    bool takeResize(float scale) {
        const bool resize = requested || (lastScale > 0.0f && lastScale != scale);
        requested = false;
        lastScale = scale;
        return resize;
    }
};
// Keep Custom selected after editing to a preset value.
struct CustomChoice {
    int value;          // the slider's value
    int seen = -99;     // the setting when last followed
    bool picked = false;
    bool follow(int setting, bool preset) {
        if (setting != seen) {
            if (preset) picked = false;
            else value = setting;
            seen = setting;
        }
        return picked || !preset;
    }
    void chose(int setting) { picked = true; seen = setting; }   // set through Custom (radio or slider)
    void other() { picked = false; }                               // another radio picked
};
} // namespace truefps
