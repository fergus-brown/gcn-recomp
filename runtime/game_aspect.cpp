// See game_aspect.h.
#include "game_aspect.h"
#include "gx/render_gl.h"
#include "input_log.h"
#include "netplay.h"

// 4:3 until the first poll, whatever the display: the same in every run.
static double g_aspect = 4.0 / 3.0;

double game_aspect() { return g_aspect; }

void game_aspect_poll() {
    float a = (float)(netplay_active() ? netplay_aspect() : gx::render_aspect());
    bool valid = true;
    input_pose("aspect", &valid, &a, 1);
    if (valid && a > 0.0f) g_aspect = a;
}
