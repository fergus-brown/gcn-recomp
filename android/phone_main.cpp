// A phone or tablet's shell: the game on the device's screen through EGL and GL ES, its
// sound through AAudio, a Bluetooth or USB gamepad as the controller, and, with none, an
// on-screen one drawn over the picture. The headset's shell is openxr_main.cpp; this one
// has no OpenXR, and is built in its place with -DGCN_ANDROID_PHONE=ON.
//
// The app's external files directory, /sdcard/Android/data/<package>/files, holds:
//   game.iso (or game.ciso)  the user's disc image (package-apk.ps1 pushes it)
//   memcard_a.raw            the memory card, written by the game
//   gcn_netplay.txt          optional: "host <port> <players>" or "join <address:port>"
//                            (netplay.h); the session starts before the game boots
//   gcn_env.txt, gcn_input.txt  as for the headset (environment, scripted input)
#include "runtime.h"
#include "platform.h"
#include "input_script.h"
#include "input_log.h"
#include "netplay.h"

#include "gx/render_gl.h"
#include "gx/gl.h"

#include <android/log.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <jni.h>

#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

#define TAG GCN_LOG_TAG
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

uint32_t boot_load(const char* iso_path);
bool audio_open();

namespace {

// ---------------------------------------------------------------------------
// printf and fprintf go nowhere in an app: pipe them into logcat.
// ---------------------------------------------------------------------------
void* log_pump(void*) {
    int fds[2];
    if (pipe(fds) != 0) return nullptr;
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf) - 1)) > 0) {
        if (buf[n - 1] == '\n') n--;
        buf[n] = 0;
        __android_log_write(ANDROID_LOG_INFO, TAG, buf);
    }
    return nullptr;
}

[[noreturn]] void app_exit(const char* why) {
    LOGI("exiting: %s", why);
    fflush(nullptr);
    plat_exit_now(0);
}

// ---------------------------------------------------------------------------
// EGL: one context for the whole run, and a window surface for as long as the activity
// has a window (it loses it in the background).
// ---------------------------------------------------------------------------
EGLDisplay g_dpy = EGL_NO_DISPLAY;
EGLContext g_ctx = EGL_NO_CONTEXT;
EGLSurface g_surf = EGL_NO_SURFACE;
EGLSurface g_pbuf = EGL_NO_SURFACE;
EGLConfig g_cfg;
int g_w = 0, g_h = 0;

void* gl_proc(const char* name) {
    static void* lib = [] {
        void* h = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
        if (!h) h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_LOCAL);
        return h;
    }();
    if (lib)
        if (void* p = dlsym(lib, name)) return p;
    return (void*)eglGetProcAddress(name);
}

bool egl_init() {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_dpy, nullptr, nullptr)) { LOGE("eglInitialize failed"); return false; }
    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) { LOGE("no ES3 config"); return false; }
    // The version the renderer was built for (gx/gl.h): 3.2, or 3.0 with GCN_GL_ES30.
    const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, GCN_GL_MAJOR, EGL_CONTEXT_MINOR_VERSION, GCN_GL_MINOR, EGL_NONE};
    g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: this device has no GL ES %d.%d (an emulator stops at 3.1: build with "
             "-DGCN_GL_ES30=ON)", GCN_GL_MAJOR, GCN_GL_MINOR);
        return false;
    }
    // A pbuffer to keep the context current with while there is no window.
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g_pbuf = eglCreatePbufferSurface(g_dpy, g_cfg, pb);
    if (!eglMakeCurrent(g_dpy, g_pbuf, g_pbuf, g_ctx)) { LOGE("eglMakeCurrent failed"); return false; }
    return true;
}

void window_attach(ANativeWindow* win) {
    if (g_surf != EGL_NO_SURFACE || !win) return;
    g_surf = eglCreateWindowSurface(g_dpy, g_cfg, win, nullptr);
    if (g_surf == EGL_NO_SURFACE) { LOGE("eglCreateWindowSurface failed"); return; }
    eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx);
    eglSwapInterval(g_dpy, 1);
    eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &g_w);
    eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &g_h);
    LOGI("window %dx%d", g_w, g_h);
}

void window_detach() {
    if (g_surf == EGL_NO_SURFACE) return;
    eglMakeCurrent(g_dpy, g_pbuf, g_pbuf, g_ctx);
    eglDestroySurface(g_dpy, g_surf);
    g_surf = EGL_NO_SURFACE;
}

// ---------------------------------------------------------------------------
// The controller: a gamepad when one is in use, else the on-screen controls.
// ---------------------------------------------------------------------------
std::mutex g_pad_mutex;
PadState g_gamepad;                 // the gamepad's state as its events left it
bool g_gamepad_used = false;        // a gamepad has been used: the on-screen controls hide
float g_lt = 0, g_rt = 0;           // analog triggers, 0..1

uint8_t axis_u8(float v, bool invert) {
    if (invert) v = -v;
    const int x = (int)std::lround(128.0f + v * 127.0f);
    return (uint8_t)std::clamp(x, 0, 255);
}

// The gamepad's buttons in GameCube terms, by the physical position Android names them for:
// bottom A, left X -> B, right B -> X, top Y, as the desktop maps a pad.
uint16_t key_button(int32_t code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return PAD_A;
    case AKEYCODE_BUTTON_X: return PAD_B;
    case AKEYCODE_BUTTON_B: return PAD_X;
    case AKEYCODE_BUTTON_Y: return PAD_Y;
    case AKEYCODE_BUTTON_START: return PAD_START;
    case AKEYCODE_BUTTON_R1: return PAD_Z;
    case AKEYCODE_BUTTON_L1: return PAD_Z;
    case AKEYCODE_BUTTON_L2: return PAD_L;
    case AKEYCODE_BUTTON_R2: return PAD_R;
    case AKEYCODE_DPAD_UP: return PAD_UP;
    case AKEYCODE_DPAD_DOWN: return PAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return PAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return PAD_RIGHT;
    default: return 0;
    }
}

// ---- on-screen controls ----
// Positions in units of the screen's height, from the left or right edge.
struct Control {
    const char* name;
    bool from_right;
    float x, y, r;          // centre and radius, in screen heights
    uint16_t button;        // 0 for the stick
    float cr, cg, cb;
};
const Control kControls[] = {
    {"stick", false, 0.22f, 0.70f, 0.17f, 0, 0.8f, 0.8f, 0.8f},
    {"A", true, 0.20f, 0.72f, 0.085f, PAD_A, 0.2f, 0.8f, 0.5f},
    {"B", true, 0.36f, 0.82f, 0.06f, PAD_B, 0.9f, 0.25f, 0.25f},
    {"X", true, 0.07f, 0.58f, 0.06f, PAD_X, 0.85f, 0.85f, 0.85f},
    {"Y", true, 0.24f, 0.50f, 0.06f, PAD_Y, 0.85f, 0.85f, 0.85f},
    {"Z", true, 0.08f, 0.33f, 0.05f, PAD_Z, 0.55f, 0.35f, 0.85f},
    {"R", true, 0.08f, 0.12f, 0.06f, PAD_R, 0.7f, 0.7f, 0.7f},
    {"L", false, 0.08f, 0.12f, 0.06f, PAD_L, 0.7f, 0.7f, 0.7f},
    {"START", false, 0.0f, 0.10f, 0.045f, PAD_START, 0.7f, 0.7f, 0.7f},  // x: the screen's middle
};
constexpr int kNumControls = sizeof(kControls) / sizeof(kControls[0]);

struct Touch { int32_t id = -1; int control = -1; float x0 = 0, y0 = 0, x = 0, y = 0; };
Touch g_touches[10];

void control_centre(int i, float* cx, float* cy, float* r) {
    const Control& c = kControls[i];
    const float h = (float)g_h;
    *r = c.r * h;
    *cy = c.y * h;
    if (!strcmp(c.name, "START")) *cx = g_w * 0.5f;
    else *cx = c.from_right ? g_w - c.x * h : c.x * h;
}

int control_at(float x, float y) {
    for (int i = 0; i < kNumControls; i++) {
        float cx, cy, r;
        control_centre(i, &cx, &cy, &r);
        const float grow = i == 0 ? 1.6f : 1.35f;  // fingers are bigger than the circles
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r * grow * grow) return i;
    }
    // Anywhere in the left half not on a button moves the stick from where it went down.
    return x < g_w * 0.5f ? 0 : -1;
}

PadState touch_state() {
    PadState p;
    p.connected = true;
    for (const Touch& t : g_touches) {
        if (t.id < 0 || t.control < 0) continue;
        const Control& c = kControls[t.control];
        if (c.button) {
            p.buttons |= c.button;
            if (c.button == PAD_L) p.trig_l = 255;
            if (c.button == PAD_R) p.trig_r = 255;
        } else {
            float cx, cy, r;
            control_centre(0, &cx, &cy, &r);
            float dx = (t.x - t.x0) / r, dy = (t.y - t.y0) / r;
            const float len = std::sqrt(dx * dx + dy * dy);
            if (len > 1.0f) { dx /= len; dy /= len; }
            p.stick_x = axis_u8(dx, false);
            p.stick_y = axis_u8(dy, true);
        }
    }
    return p;
}

int32_t on_input(android_app*, AInputEvent* e) {
    const int32_t type = AInputEvent_getType(e);
    const int32_t source = AInputEvent_getSource(e);
    std::lock_guard<std::mutex> lk(g_pad_mutex);
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(e);
        if (code == AKEYCODE_BACK) return 1;  // swallowed: Back would end the game
        const uint16_t b = key_button(code);
        if (!b) return 0;
        g_gamepad_used = true;
        if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN) g_gamepad.buttons |= b;
        else if (AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_UP) g_gamepad.buttons &= ~b;
        if (b == PAD_L) g_lt = (g_gamepad.buttons & PAD_L) ? 1.0f : 0.0f;
        if (b == PAD_R) g_rt = (g_gamepad.buttons & PAD_R) ? 1.0f : 0.0f;
        return 1;
    }
    if (type != AINPUT_EVENT_TYPE_MOTION) return 0;
    if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
        g_gamepad_used = true;
        auto ax = [&](int32_t a) { return AMotionEvent_getAxisValue(e, a, 0); };
        const float lx = ax(AMOTION_EVENT_AXIS_X), ly = ax(AMOTION_EVENT_AXIS_Y);
        const float rx = ax(AMOTION_EVENT_AXIS_Z), ry = ax(AMOTION_EVENT_AXIS_RZ);
        g_gamepad.stick_x = std::fabs(lx) > 0.12f ? axis_u8(lx, false) : 0x80;
        g_gamepad.stick_y = std::fabs(ly) > 0.12f ? axis_u8(ly, true) : 0x80;
        g_gamepad.cstick_x = std::fabs(rx) > 0.12f ? axis_u8(rx, false) : 0x80;
        g_gamepad.cstick_y = std::fabs(ry) > 0.12f ? axis_u8(ry, true) : 0x80;
        const float lt = std::max(ax(AMOTION_EVENT_AXIS_LTRIGGER), ax(AMOTION_EVENT_AXIS_BRAKE));
        const float rt = std::max(ax(AMOTION_EVENT_AXIS_RTRIGGER), ax(AMOTION_EVENT_AXIS_GAS));
        g_lt = lt; g_rt = rt;
        // A pad whose d-pad is a hat reports it as axes.
        const float hx = ax(AMOTION_EVENT_AXIS_HAT_X), hy = ax(AMOTION_EVENT_AXIS_HAT_Y);
        g_gamepad.buttons &= ~(PAD_LEFT | PAD_RIGHT | PAD_UP | PAD_DOWN);
        if (hx < -0.5f) g_gamepad.buttons |= PAD_LEFT;
        if (hx > 0.5f) g_gamepad.buttons |= PAD_RIGHT;
        if (hy < -0.5f) g_gamepad.buttons |= PAD_UP;
        if (hy > 0.5f) g_gamepad.buttons |= PAD_DOWN;
        return 1;
    }
    if ((source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) return 0;
    g_gamepad_used = false;  // touching the screen brings the on-screen controls back
    const int32_t action = AMotionEvent_getAction(e);
    const int32_t kind = action & AMOTION_EVENT_ACTION_MASK;
    const size_t idx = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    auto find = [](int32_t id) -> Touch* {
        for (Touch& t : g_touches) if (t.id == id) return &t;
        return nullptr;
    };
    if (kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN) {
        const int32_t id = AMotionEvent_getPointerId(e, idx);
        for (Touch& t : g_touches)
            if (t.id < 0) {
                t.id = id;
                t.x0 = t.x = AMotionEvent_getX(e, idx);
                t.y0 = t.y = AMotionEvent_getY(e, idx);
                t.control = control_at(t.x, t.y);
                break;
            }
    } else if (kind == AMOTION_EVENT_ACTION_MOVE) {
        for (size_t i = 0; i < AMotionEvent_getPointerCount(e); i++)
            if (Touch* t = find(AMotionEvent_getPointerId(e, i))) {
                t->x = AMotionEvent_getX(e, i);
                t->y = AMotionEvent_getY(e, i);
            }
    } else if (kind == AMOTION_EVENT_ACTION_UP || kind == AMOTION_EVENT_ACTION_POINTER_UP) {
        if (Touch* t = find(AMotionEvent_getPointerId(e, idx))) *t = Touch();
    } else if (kind == AMOTION_EVENT_ACTION_CANCEL) {
        for (Touch& t : g_touches) t = Touch();
    }
    return 1;
}

void update_pad() {
    PadState p;
    {
        std::lock_guard<std::mutex> lk(g_pad_mutex);
        if (g_gamepad_used) {
            p = g_gamepad;
            p.connected = true;
            p.trig_l = (uint8_t)std::clamp((int)(g_lt * 255.0f), 0, 255);
            p.trig_r = (uint8_t)std::clamp((int)(g_rt * 255.0f), 0, 255);
            if (g_lt > 0.85f) p.buttons |= PAD_L;
            if (g_rt > 0.85f) p.buttons |= PAD_R;
        } else {
            p = touch_state();
        }
    }
    input_script_apply(p);
    pad_set_state(0, p);
}

// ---- drawing the on-screen controls ----
// One translucent disc per control, from a program with no vertex input (the corners come
// from gl_VertexID), on a vertex array of its own; the renderer re-applies all of its state
// at its next batch, but its vertex array and program are put back here all the same.
GLuint g_ov_prog = 0, g_ov_vao = 0;
GLint g_ov_u_rect = -1, g_ov_u_col = -1;

GLuint compile(GLenum kind, const char* src) {
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("overlay shader: %s", log);
    }
    return s;
}

void overlay_init() {
    static const char* vs =
        "#version 300 es\n"
        "uniform vec4 u_rect;\n"  // centre x, y and radius, in clip space; y radius
        "out vec2 v_p;\n"
        "void main() {\n"
        "  vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;\n"
        "  v_p = c;\n"
        "  gl_Position = vec4(u_rect.xy + c * vec2(u_rect.z, u_rect.w), 0.0, 1.0);\n"
        "}\n";
    static const char* fs =
        "#version 300 es\nprecision mediump float;\n"
        "uniform vec4 u_col;\n"
        "in vec2 v_p;\nout vec4 o;\n"
        "void main() {\n"
        "  float d = length(v_p);\n"
        "  float ring = smoothstep(0.80, 0.86, d) * (1.0 - smoothstep(0.94, 1.0, d));\n"
        "  float fill = 1.0 - smoothstep(0.80, 0.86, d);\n"
        "  float a = u_col.a * (fill * 0.45 + ring);\n"
        "  if (a <= 0.0) discard;\n"
        "  o = vec4(u_col.rgb, a);\n"
        "}\n";
    g_ov_prog = glCreateProgram();
    glAttachShader(g_ov_prog, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(g_ov_prog, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(g_ov_prog);
    g_ov_u_rect = glGetUniformLocation(g_ov_prog, "u_rect");
    g_ov_u_col = glGetUniformLocation(g_ov_prog, "u_col");
    glGenVertexArrays(1, &g_ov_vao);
}

void overlay_disc(float cx, float cy, float r, float red, float green, float blue, float alpha) {
    glUniform4f(g_ov_u_rect, cx / g_w * 2.0f - 1.0f, 1.0f - cy / g_h * 2.0f, r / g_w * 2.0f, r / g_h * 2.0f);
    glUniform4f(g_ov_u_col, red, green, blue, alpha);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void overlay_draw() {
    bool hidden;
    Touch touches[10];
    {
        std::lock_guard<std::mutex> lk(g_pad_mutex);
        hidden = g_gamepad_used;
        memcpy(touches, g_touches, sizeof(touches));
    }
    if (hidden || !g_ov_prog) return;
    GLint vao = 0, prog = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_w, g_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(g_ov_prog);
    glBindVertexArray(g_ov_vao);
    for (int i = 0; i < kNumControls; i++) {
        float cx, cy, r;
        control_centre(i, &cx, &cy, &r);
        bool down = false;
        for (const Touch& t : touches) down |= t.id >= 0 && t.control == i;
        const Control& c = kControls[i];
        overlay_disc(cx, cy, r, c.cr, c.cg, c.cb, down ? 0.75f : 0.4f);
        if (i == 0) {
            // The stick's knob, where the finger has pushed it.
            float kx = cx, ky = cy;
            for (const Touch& t : touches)
                if (t.id >= 0 && t.control == 0) {
                    float dx = t.x - t.x0, dy = t.y - t.y0;
                    const float len = std::sqrt(dx * dx + dy * dy);
                    if (len > r) { dx *= r / len; dy *= r / len; }
                    kx += dx; ky += dy;
                }
            overlay_disc(kx, ky, r * 0.45f, 0.95f, 0.95f, 0.95f, 0.6f);
        }
    }
    glBindVertexArray((GLuint)vao);
    glUseProgram((GLuint)prog);
}

// ---------------------------------------------------------------------------
// The app around the game.
// ---------------------------------------------------------------------------
android_app* g_app;
std::atomic<bool> g_have_window{false};

void on_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        window_attach(app->window);
        g_have_window = g_surf != EGL_NO_SURFACE;
        break;
    case APP_CMD_TERM_WINDOW:
        g_have_window = false;
        window_detach();
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        if (g_surf != EGL_NO_SURFACE) {
            eglQuerySurface(g_dpy, g_surf, EGL_WIDTH, &g_w);
            eglQuerySurface(g_dpy, g_surf, EGL_HEIGHT, &g_h);
        }
        break;
    default:
        break;
    }
}

bool pump(int timeout_ms) {
    int events;
    android_poll_source* src;
    while (ALooper_pollOnce(timeout_ms, nullptr, &events, (void**)&src) >= 0) {
        if (src) src->process(g_app, src);
        if (g_app->destroyRequested) return false;
        timeout_ms = 0;
    }
    return !g_app->destroyRequested;
}

std::string read_text(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// gcn_env.txt: KEY=VALUE lines put into the environment, standing in for the shell an
// app launched from the home screen does not have.
void apply_env_file(const std::string& dir) {
    FILE* f = fopen((dir + "/gcn_env.txt").c_str(), "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const size_t eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos || eq == 0) continue;
        setenv(l.substr(0, eq).c_str(), l.substr(eq + 1).c_str(), 1);
        LOGI("env: %s", l.c_str());
    }
    fclose(f);
}

// Full screen with the system bars hidden until swiped in (immersive sticky), through the
// activity's decor view; best effort, since the game plays either way.
void go_immersive() {
    JavaVM* vm = g_app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jobject act = g_app->activity->clazz;
    jclass act_cls = env->GetObjectClass(act);
    jobject window = env->CallObjectMethod(act, env->GetMethodID(act_cls, "getWindow", "()Landroid/view/Window;"));
    if (window && !env->ExceptionCheck()) {
        jobject decor = env->CallObjectMethod(window, env->GetMethodID(env->GetObjectClass(window), "getDecorView",
                                                                       "()Landroid/view/View;"));
        if (decor && !env->ExceptionCheck()) {
            // SYSTEM_UI_FLAG_IMMERSIVE_STICKY | FULLSCREEN | HIDE_NAVIGATION | LAYOUT_*
            const jint flags = 0x1000 | 0x4 | 0x2 | 0x400 | 0x200 | 0x100;
            env->CallVoidMethod(decor, env->GetMethodID(env->GetObjectClass(decor), "setSystemUiVisibility", "(I)V"), flags);
        }
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    vm->DetachCurrentThread();
}

// The game reset the console: start the app again in a fresh process, as the headset's
// shell does (a recompiled game has no way back to its start inside one process).
void relaunch(uint32_t, uint32_t) {
    JavaVM* vm = g_app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jobject act = g_app->activity->clazz;
    jclass act_cls = env->GetObjectClass(act);
    jobject pm = env->CallObjectMethod(act, env->GetMethodID(act_cls, "getPackageManager", "()Landroid/content/pm/PackageManager;"));
    jobject pkg = env->CallObjectMethod(act, env->GetMethodID(act_cls, "getPackageName", "()Ljava/lang/String;"));
    jobject intent = env->CallObjectMethod(
        pm, env->GetMethodID(env->GetObjectClass(pm), "getLaunchIntentForPackage", "(Ljava/lang/String;)Landroid/content/Intent;"), pkg);
    if (env->ExceptionCheck() || !intent) { env->ExceptionClear(); return; }
    const jint kNewTask = 0x10000000, kClearTask = 0x00008000;
    env->CallObjectMethod(intent, env->GetMethodID(env->GetObjectClass(intent), "addFlags", "(I)Landroid/content/Intent;"),
                          kNewTask | kClearTask);
    env->CallVoidMethod(act, env->GetMethodID(act_cls, "startActivity", "(Landroid/content/Intent;)V"), intent);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

}  // namespace

void android_main(android_app* app) {
    // A second android_main in one process would meet the first one's game; end instead.
    static bool entered;
    if (entered) app_exit("android_main re-entered; this process has already run a game");
    entered = true;
    g_app = app;

    pthread_t t;
    pthread_create(&t, nullptr, log_pump, nullptr);
    pthread_detach(t);
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    go_immersive();

    const std::string dir = app->activity->externalDataPath ? app->activity->externalDataPath : "";
    std::string iso = dir + "/game.iso";
    if (!plat_readable(iso.c_str())) iso = dir + "/game.ciso";

    if (!egl_init()) app_exit("EGL init failed");
    const int glver = gl_load_with(gl_proc);
    if (glver < GCN_GL_VERSION_MIN) { LOGE("GL ES %d.%d too old", glver / 10, glver % 10); app_exit("GL too old"); }
    LOGI("GL %s / %s", (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));
    if (!plat_readable(iso.c_str())) {
        LOGE("no game image at %s/game.iso or game.ciso", dir.c_str());
        app_exit("no game image");
    }

    // The window first: a session's handshake below can take a while.
    while (!g_have_window) if (!pump(100)) app_exit("activity destroyed");

    apply_env_file(dir);
    set_reset_handler(relaunch);
    mem_init();
    timing_init();
    input_script_init(read_text(dir + "/gcn_input.txt").c_str());
    static const std::string shader_cache = dir + "/shaders.bin";
    gx::render_set_shader_cache(shader_cache.c_str());
    gx::render_set_async_shaders(true);
    gx::render_init(getenv("GCN_SCALE") ? atoi(getenv("GCN_SCALE")) : 2);
    overlay_init();

    static std::string save_dir = dir;
    if (!save_dir.empty()) g_save_dir = save_dir.c_str();
    static std::string card = dir + "/memcard_a.raw";

    // A session, when gcn_netplay.txt asks for one: "host <port> <players> [delay]" or
    // "join <address:port>". A client plays on the host's card, in a scratch file.
    static std::string session_card;
    const std::string net = read_text(dir + "/gcn_netplay.txt");
    if (!net.empty()) {
        char verb[16] = "", arg[128] = "";
        int players = 2, delay = 6;
        sscanf(net.c_str(), "%15s %127s %d %d", verb, arg, &players, &delay);
        if (!strcmp(verb, "host")) {
            if (!netplay_host(atoi(arg), players, delay, gx::render_aspect(), card)) LOGE("netplay: could not host; playing alone");
        } else if (!strcmp(verb, "join")) {
            session_card = dir + "/memcard_session.raw";
            if (netplay_join(arg, gx::render_aspect(), session_card)) g_memcard_path = session_card.c_str();
            else LOGE("netplay: could not join %s; playing alone", arg);
        }
    }

    static std::string replay_card;
    if (const char* r = getenv("GCN_REPLAY")) {
        const std::string replay_dir = r[0] == '/' ? std::string(r) : dir + "/" + r;
        if (!input_replay_load(replay_dir, replay_card)) LOGE("no replay at %s", replay_dir.c_str());
        else if (!replay_card.empty()) g_memcard_path = replay_card.c_str();
    }
    if (!getenv("GCN_NO_INPUT_LOG")) {
        char stamp[64];
        time_t now = time(nullptr);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
        input_log_start(dir + "/inputs/" + stamp,
                        !replay_card.empty() ? replay_card : g_memcard_path ? std::string(g_memcard_path) : card);
    }
    if (!audio_open()) LOGE("audio unavailable; continuing without sound");

    const uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);
    LOGI("game started");

    plat_thread_role(ThreadRole::RenderMain, "render");
    for (;;) {
        if (!pump(g_have_window ? 0 : 100)) break;
        update_pad();
        if (!g_have_window) continue;  // in the background: the game waits on the renderer
        gx::render_set_window_size(g_w, g_h);
        auto b = gx::take_batch(4);
        if (!b) continue;
        if (gx::render_execute(*b)) {
            overlay_draw();
            eglSwapBuffers(g_dpy, g_surf);
        }
    }
    input_log_end();
    app_exit("activity destroyed");
}
