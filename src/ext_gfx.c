/*
 * EigenScript Graphics Extension — SDL2 builtins.
 * Dynamically loads libSDL2 at runtime (no dev headers needed).
 *
 * Builtins: gfx_open, gfx_close, gfx_clear, gfx_rect, gfx_line,
 *           gfx_point, gfx_circle, gfx_present, gfx_poll, gfx_ticks, gfx_delay
 */

#include "eigenscript.h"

#if EIGENSCRIPT_EXT_GFX

#include <dlfcn.h>

/* ---- SDL2 types (minimal, no headers needed) ---- */

typedef uint8_t  Uint8;
typedef uint32_t Uint32;
typedef int32_t  Sint32;

typedef struct { int x, y, w, h; } SDL_Rect;
typedef struct { Sint32 scancode; Sint32 sym; uint16_t mod; Uint32 unused; } SDL_Keysym;
typedef struct { Uint32 type; Uint32 ts; Uint32 wid; Uint8 state; Uint8 rep; Uint8 p2; Uint8 p3; SDL_Keysym keysym; } SDL_KeyboardEvent;
typedef struct { Uint32 type; Uint32 ts; Uint32 wid; Uint32 which; Sint32 x; Sint32 y; Sint32 xrel; Sint32 yrel; } SDL_MouseMotionEvent;
typedef struct { Uint32 type; Uint32 ts; Uint32 wid; Uint32 which; Uint8 button; Uint8 state; Uint8 clicks; Uint8 p1; Sint32 x; Sint32 y; } SDL_MouseButtonEvent;
typedef struct { Uint32 type; Uint32 ts; Uint32 wid; Uint32 which; Sint32 x; Sint32 y; Uint32 direction; } SDL_MouseWheelEvent;
typedef struct { Uint32 type; Uint32 ts; Uint32 wid; Uint8 event; Uint8 p1; Uint8 p2; Uint8 p3; Sint32 data1; Sint32 data2; } SDL_WindowEvent;

typedef union {
    Uint32 type;
    SDL_KeyboardEvent key;
    SDL_MouseMotionEvent motion;
    SDL_MouseButtonEvent button;
    SDL_MouseWheelEvent wheel;
    SDL_WindowEvent window;
    Uint8 padding[64];
} SDL_Event;

typedef void SDL_Window;
typedef void SDL_Renderer;

typedef struct {
    int freq;
    uint16_t format;
    uint8_t channels;
    uint8_t silence;
    uint16_t samples;
    uint16_t padding;
    uint32_t size;
    void (*callback)(void*, uint8_t*, int);
    void *userdata;
} SDL_AudioSpec;

/* SDL2 constants */
#define MY_SDL_INIT_VIDEO       0x00000020u
#define MY_SDL_INIT_AUDIO       0x00000010u
#define MY_SDL_AUDIO_S16LSB     0x8010
#define MY_SDL_QUIT_EVENT       0x100u
#define MY_SDL_KEYDOWN          0x300u
#define MY_SDL_KEYUP            0x301u
#define MY_SDL_MOUSEMOTION      0x400u
#define MY_SDL_MOUSEBUTTONDOWN  0x401u
#define MY_SDL_MOUSEBUTTONUP    0x402u
#define MY_SDL_MOUSEWHEEL       0x403u
#define MY_SDL_WINDOWEVENT      0x200u
#define MY_SDL_WINDOWPOS_CENTERED 0x2FFF0000
#define MY_SDL_WINDOW_RESIZABLE 0x00000020u
#define MY_SDL_RENDERER_ACCELERATED 0x02u
#define MY_SDL_RENDERER_PRESENTVSYNC 0x04u
#define MY_SDL_BLENDMODE_BLEND  0x01u

/* ---- Function pointers ---- */

static void *g_sdl_lib = NULL;
static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;

static int (*p_SDL_Init)(Uint32);
static void (*p_SDL_Quit)(void);
static const char* (*p_SDL_GetError)(void);
static SDL_Window* (*p_SDL_CreateWindow)(const char*, int, int, int, int, Uint32);
static void (*p_SDL_DestroyWindow)(SDL_Window*);
static void (*p_SDL_SetWindowTitle)(SDL_Window*, const char*);
static SDL_Renderer* (*p_SDL_CreateRenderer)(SDL_Window*, int, Uint32);
static void (*p_SDL_DestroyRenderer)(SDL_Renderer*);
static int (*p_SDL_SetRenderDrawColor)(SDL_Renderer*, Uint8, Uint8, Uint8, Uint8);
static int (*p_SDL_SetRenderDrawBlendMode)(SDL_Renderer*, int);
static int (*p_SDL_RenderClear)(SDL_Renderer*);
static int (*p_SDL_RenderFillRect)(SDL_Renderer*, const SDL_Rect*);
static int (*p_SDL_RenderDrawLine)(SDL_Renderer*, int, int, int, int);
static int (*p_SDL_RenderDrawPoint)(SDL_Renderer*, int, int);
static void (*p_SDL_RenderPresent)(SDL_Renderer*);
static int (*p_SDL_PollEvent)(SDL_Event*);
static Uint32 (*p_SDL_GetTicks)(void);
static void (*p_SDL_Delay)(Uint32);
static int (*p_SDL_RenderSetClipRect)(SDL_Renderer*, const SDL_Rect*);

/* Audio function pointers */
static int (*p_SDL_OpenAudioDevice)(const char*, int, const SDL_AudioSpec*, SDL_AudioSpec*, int);
static void (*p_SDL_CloseAudioDevice)(int);
static int (*p_SDL_QueueAudio)(int, const void*, Uint32);
static void (*p_SDL_PauseAudioDevice)(int, int);
static Uint32 (*p_SDL_GetQueuedAudioSize)(int);
static void (*p_SDL_ClearQueuedAudio)(int);

/* Audio state */
static int g_audio_device = 0;
static int g_audio_freq = 44100;
static int g_audio_channels = 1;

static int load_sdl2(void) {
    if (g_sdl_lib) return 1;
    g_sdl_lib = dlopen("libSDL2-2.0.so.0", RTLD_LAZY);
    if (!g_sdl_lib) g_sdl_lib = dlopen("libSDL2.so", RTLD_LAZY);
    if (!g_sdl_lib) return 0;

    int ok = 1;
    #define LOAD(name) do { \
        p_##name = dlsym(g_sdl_lib, #name); \
        if (!p_##name) { fprintf(stderr, "gfx: missing symbol %s\n", #name); ok = 0; } \
    } while(0)
    LOAD(SDL_Init); LOAD(SDL_Quit); LOAD(SDL_GetError);
    LOAD(SDL_CreateWindow); LOAD(SDL_DestroyWindow); LOAD(SDL_SetWindowTitle);
    LOAD(SDL_CreateRenderer); LOAD(SDL_DestroyRenderer);
    LOAD(SDL_SetRenderDrawColor); LOAD(SDL_SetRenderDrawBlendMode);
    LOAD(SDL_RenderClear); LOAD(SDL_RenderFillRect);
    LOAD(SDL_RenderDrawLine); LOAD(SDL_RenderDrawPoint);
    LOAD(SDL_RenderPresent); LOAD(SDL_PollEvent);
    LOAD(SDL_GetTicks); LOAD(SDL_Delay);
    #undef LOAD
    /* Optional symbols — NULL is fine */
    p_SDL_RenderSetClipRect = dlsym(g_sdl_lib, "SDL_RenderSetClipRect");
    p_SDL_OpenAudioDevice = dlsym(g_sdl_lib, "SDL_OpenAudioDevice");
    p_SDL_CloseAudioDevice = dlsym(g_sdl_lib, "SDL_CloseAudioDevice");
    p_SDL_QueueAudio = dlsym(g_sdl_lib, "SDL_QueueAudio");
    p_SDL_PauseAudioDevice = dlsym(g_sdl_lib, "SDL_PauseAudioDevice");
    p_SDL_GetQueuedAudioSize = dlsym(g_sdl_lib, "SDL_GetQueuedAudioSize");
    p_SDL_ClearQueuedAudio = dlsym(g_sdl_lib, "SDL_ClearQueuedAudio");
    if (!ok) {
        dlclose(g_sdl_lib);
        g_sdl_lib = NULL;
        return 0;
    }
    return 1;
}

/* Scancode to key name — SDL2 scancodes */
static const char* scancode_name(int sc) {
    switch (sc) {
        case 4: return "a"; case 5: return "b"; case 6: return "c";
        case 7: return "d"; case 8: return "e"; case 9: return "f";
        case 10: return "g"; case 11: return "h"; case 12: return "i";
        case 13: return "j"; case 14: return "k"; case 15: return "l";
        case 16: return "m"; case 17: return "n"; case 18: return "o";
        case 19: return "p"; case 20: return "q"; case 21: return "r";
        case 22: return "s"; case 23: return "t"; case 24: return "u";
        case 25: return "v"; case 26: return "w"; case 27: return "x";
        case 28: return "y"; case 29: return "z";
        case 30: return "1"; case 31: return "2"; case 32: return "3";
        case 33: return "4"; case 34: return "5"; case 35: return "6";
        case 36: return "7"; case 37: return "8"; case 38: return "9";
        case 39: return "0";
        case 40: return "return"; case 41: return "escape";
        case 42: return "backspace"; case 43: return "tab";
        case 44: return "space";
        case 45: return "-"; case 46: return "=";
        case 47: return "["; case 48: return "]"; case 49: return "\\";
        case 51: return ";"; case 52: return "'";
        case 54: return ","; case 55: return "."; case 56: return "/";
        case 57: return "capslock";
        case 58: return "f1"; case 59: return "f2"; case 60: return "f3";
        case 61: return "f4"; case 62: return "f5"; case 63: return "f6";
        case 64: return "f7"; case 65: return "f8"; case 66: return "f9";
        case 67: return "f10"; case 68: return "f11"; case 69: return "f12";
        case 73: return "insert"; case 74: return "home";
        case 75: return "pageup"; case 76: return "delete";
        case 77: return "end"; case 78: return "pagedown";
        case 79: return "right"; case 80: return "left";
        case 81: return "down"; case 82: return "up";
        default: return "";
    }
}

/* ---- Builtins ---- */

/* gfx_open of [width, height, title] */
Value* builtin_gfx_open(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 3) return make_num(0);
    int w = (int)arg->data.list.items[0]->data.num;
    int h = (int)arg->data.list.items[1]->data.num;
    const char *title = arg->data.list.items[2]->type == VAL_STR ? arg->data.list.items[2]->data.str : "EigenScript";

    if (!load_sdl2()) {
        fprintf(stderr, "gfx_open: cannot load libSDL2\n");
        return make_num(0);
    }
    if (p_SDL_Init(MY_SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "gfx_open: SDL_Init failed: %s\n", p_SDL_GetError());
        return make_num(0);
    }
    g_window = p_SDL_CreateWindow(title, MY_SDL_WINDOWPOS_CENTERED, MY_SDL_WINDOWPOS_CENTERED, w, h, MY_SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        fprintf(stderr, "gfx_open: SDL_CreateWindow failed: %s\n", p_SDL_GetError());
        return make_num(0);
    }
    g_renderer = p_SDL_CreateRenderer(g_window, -1, MY_SDL_RENDERER_ACCELERATED | MY_SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) {
        g_renderer = p_SDL_CreateRenderer(g_window, -1, 0);
    }
    if (!g_renderer) {
        fprintf(stderr, "gfx_open: SDL_CreateRenderer failed\n");
        p_SDL_DestroyWindow(g_window);
        g_window = NULL;
        return make_num(0);
    }
    p_SDL_SetRenderDrawBlendMode(g_renderer, MY_SDL_BLENDMODE_BLEND);
    return make_num(1);
}

/* gfx_close of null */
Value* builtin_gfx_close(Value *arg) {
    (void)arg;
    if (g_audio_device) { p_SDL_CloseAudioDevice(g_audio_device); g_audio_device = 0; }
    if (g_renderer) { p_SDL_DestroyRenderer(g_renderer); g_renderer = NULL; }
    if (g_window) { p_SDL_DestroyWindow(g_window); g_window = NULL; }
    if (g_sdl_lib) {
        p_SDL_Quit();
        dlclose(g_sdl_lib);
        g_sdl_lib = NULL;
    }
    return make_null();
}

/* gfx_clear of [r, g, b] */
Value* builtin_gfx_clear(Value *arg) {
    if (!g_renderer) return make_null();
    int r = 0, g = 0, b = 0;
    if (arg && arg->type == VAL_LIST && arg->data.list.count >= 3) {
        r = (int)arg->data.list.items[0]->data.num;
        g = (int)arg->data.list.items[1]->data.num;
        b = (int)arg->data.list.items[2]->data.num;
    }
    p_SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    p_SDL_RenderClear(g_renderer);
    return make_null();
}

/* gfx_rect of [x, y, w, h, r, g, b] or [x, y, w, h, r, g, b, a] */
Value* builtin_gfx_rect(Value *arg) {
    if (!g_renderer || !arg || arg->type != VAL_LIST || arg->data.list.count < 7) return make_null();
    SDL_Rect rect;
    rect.x = (int)arg->data.list.items[0]->data.num;
    rect.y = (int)arg->data.list.items[1]->data.num;
    rect.w = (int)arg->data.list.items[2]->data.num;
    rect.h = (int)arg->data.list.items[3]->data.num;
    int r = (int)arg->data.list.items[4]->data.num;
    int g = (int)arg->data.list.items[5]->data.num;
    int b = (int)arg->data.list.items[6]->data.num;
    int a = (arg->data.list.count >= 8) ? (int)arg->data.list.items[7]->data.num : 255;
    p_SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    p_SDL_RenderFillRect(g_renderer, &rect);
    return make_null();
}

/* gfx_line of [x1, y1, x2, y2, r, g, b] */
Value* builtin_gfx_line(Value *arg) {
    if (!g_renderer || !arg || arg->type != VAL_LIST || arg->data.list.count < 7) return make_null();
    int x1 = (int)arg->data.list.items[0]->data.num;
    int y1 = (int)arg->data.list.items[1]->data.num;
    int x2 = (int)arg->data.list.items[2]->data.num;
    int y2 = (int)arg->data.list.items[3]->data.num;
    int r = (int)arg->data.list.items[4]->data.num;
    int g = (int)arg->data.list.items[5]->data.num;
    int b = (int)arg->data.list.items[6]->data.num;
    p_SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    p_SDL_RenderDrawLine(g_renderer, x1, y1, x2, y2);
    return make_null();
}

/* gfx_point of [x, y, r, g, b] */
Value* builtin_gfx_point(Value *arg) {
    if (!g_renderer || !arg || arg->type != VAL_LIST || arg->data.list.count < 5) return make_null();
    int x = (int)arg->data.list.items[0]->data.num;
    int y = (int)arg->data.list.items[1]->data.num;
    int r = (int)arg->data.list.items[2]->data.num;
    int g = (int)arg->data.list.items[3]->data.num;
    int b = (int)arg->data.list.items[4]->data.num;
    p_SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    p_SDL_RenderDrawPoint(g_renderer, x, y);
    return make_null();
}

/* gfx_circle of [cx, cy, radius, r, g, b] — filled circle via midpoint */
Value* builtin_gfx_circle(Value *arg) {
    if (!g_renderer || !arg || arg->type != VAL_LIST || arg->data.list.count < 6) return make_null();
    int cx = (int)arg->data.list.items[0]->data.num;
    int cy = (int)arg->data.list.items[1]->data.num;
    int radius = (int)arg->data.list.items[2]->data.num;
    int r = (int)arg->data.list.items[3]->data.num;
    int g = (int)arg->data.list.items[4]->data.num;
    int b = (int)arg->data.list.items[5]->data.num;
    int a = (arg->data.list.count >= 7) ? (int)arg->data.list.items[6]->data.num : 255;
    p_SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    /* Filled circle via horizontal lines */
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = (int)sqrt((double)(radius * radius - dy * dy));
        SDL_Rect row = { cx - dx, cy + dy, dx * 2 + 1, 1 };
        p_SDL_RenderFillRect(g_renderer, &row);
    }
    return make_null();
}

/* gfx_rrect of [x, y, w, h, radius, r, g, b] or [..., a]
 * Filled rounded rectangle. Draws corner arcs via scanlines + rects for body. */
Value* builtin_gfx_rrect(Value *arg) {
    if (!g_renderer || !arg || arg->type != VAL_LIST || arg->data.list.count < 8) return make_null();
    int x = (int)arg->data.list.items[0]->data.num;
    int y = (int)arg->data.list.items[1]->data.num;
    int w = (int)arg->data.list.items[2]->data.num;
    int h = (int)arg->data.list.items[3]->data.num;
    int rad = (int)arg->data.list.items[4]->data.num;
    int r = (int)arg->data.list.items[5]->data.num;
    int g = (int)arg->data.list.items[6]->data.num;
    int b = (int)arg->data.list.items[7]->data.num;
    int a = (arg->data.list.count >= 9) ? (int)arg->data.list.items[8]->data.num : 255;
    if (w <= 0 || h <= 0) return make_null();
    /* Clamp radius to half the smaller dimension */
    if (rad > w / 2) rad = w / 2;
    if (rad > h / 2) rad = h / 2;
    if (rad < 0) rad = 0;
    p_SDL_SetRenderDrawColor(g_renderer, r, g, b, a);
    if (rad == 0) {
        SDL_Rect rect = { x, y, w, h };
        p_SDL_RenderFillRect(g_renderer, &rect);
        return make_null();
    }
    /* Center body (between top and bottom rounded bands) */
    SDL_Rect center = { x, y + rad, w, h - 2 * rad };
    p_SDL_RenderFillRect(g_renderer, &center);
    /* Top and bottom bands with rounded corners via scanlines */
    for (int dy = 0; dy < rad; dy++) {
        int dx = (int)sqrt((double)(rad * rad - (rad - dy) * (rad - dy)));
        /* Top band */
        SDL_Rect top_row = { x + rad - dx, y + dy, w - 2 * (rad - dx), 1 };
        p_SDL_RenderFillRect(g_renderer, &top_row);
        /* Bottom band */
        SDL_Rect bot_row = { x + rad - dx, y + h - 1 - dy, w - 2 * (rad - dx), 1 };
        p_SDL_RenderFillRect(g_renderer, &bot_row);
    }
    return make_null();
}

/* gfx_clip of [x, y, w, h] — set render clip rectangle.
 * gfx_clip of null — clear clip rectangle. */
Value* builtin_gfx_clip(Value *arg) {
    if (!g_renderer || !p_SDL_RenderSetClipRect) return make_null();
    if (!arg || arg->type == VAL_NULL) {
        p_SDL_RenderSetClipRect(g_renderer, NULL);
        return make_null();
    }
    if (arg->type != VAL_LIST || arg->data.list.count < 4) return make_null();
    SDL_Rect clip;
    clip.x = (int)arg->data.list.items[0]->data.num;
    clip.y = (int)arg->data.list.items[1]->data.num;
    clip.w = (int)arg->data.list.items[2]->data.num;
    clip.h = (int)arg->data.list.items[3]->data.num;
    p_SDL_RenderSetClipRect(g_renderer, &clip);
    return make_null();
}

/* gfx_present of null — flip buffer to screen */
Value* builtin_gfx_present(Value *arg) {
    (void)arg;
    if (g_renderer) p_SDL_RenderPresent(g_renderer);
    return make_null();
}

/* gfx_poll of null — return next event as dict, or null if none.
 * {"type": "keydown", "key": "up"} / {"type": "quit"} /
 * {"type": "mousemove", "x": 100, "y": 200} / etc. */
Value* builtin_gfx_poll(Value *arg) {
    (void)arg;
    if (!g_window) return make_null();
    SDL_Event ev;
    if (!p_SDL_PollEvent(&ev)) return make_null();

    Value *d = make_dict(4);
    switch (ev.type) {
        case MY_SDL_QUIT_EVENT:
            dict_set(d, "type", make_str("quit"));
            break;
        case MY_SDL_KEYDOWN:
            dict_set(d, "type", make_str("keydown"));
            dict_set(d, "key", make_str(scancode_name(ev.key.keysym.scancode)));
            dict_set(d, "scancode", make_num(ev.key.keysym.scancode));
            dict_set(d, "shift", make_num((ev.key.keysym.mod & 0x03) ? 1 : 0));
            dict_set(d, "ctrl", make_num((ev.key.keysym.mod & 0xC0) ? 1 : 0));
            dict_set(d, "alt", make_num((ev.key.keysym.mod & 0x300) ? 1 : 0));
            break;
        case MY_SDL_KEYUP:
            dict_set(d, "type", make_str("keyup"));
            dict_set(d, "key", make_str(scancode_name(ev.key.keysym.scancode)));
            dict_set(d, "scancode", make_num(ev.key.keysym.scancode));
            dict_set(d, "shift", make_num((ev.key.keysym.mod & 0x03) ? 1 : 0));
            dict_set(d, "ctrl", make_num((ev.key.keysym.mod & 0xC0) ? 1 : 0));
            dict_set(d, "alt", make_num((ev.key.keysym.mod & 0x300) ? 1 : 0));
            break;
        case MY_SDL_MOUSEMOTION:
            dict_set(d, "type", make_str("mousemove"));
            dict_set(d, "x", make_num(ev.motion.x));
            dict_set(d, "y", make_num(ev.motion.y));
            break;
        case MY_SDL_MOUSEBUTTONDOWN:
            dict_set(d, "type", make_str("mousedown"));
            dict_set(d, "button", make_num(ev.button.button));
            dict_set(d, "x", make_num(ev.button.x));
            dict_set(d, "y", make_num(ev.button.y));
            break;
        case MY_SDL_MOUSEBUTTONUP:
            dict_set(d, "type", make_str("mouseup"));
            dict_set(d, "button", make_num(ev.button.button));
            dict_set(d, "x", make_num(ev.button.x));
            dict_set(d, "y", make_num(ev.button.y));
            break;
        case MY_SDL_MOUSEWHEEL:
            dict_set(d, "type", make_str("wheel"));
            dict_set(d, "x", make_num(ev.wheel.x));
            dict_set(d, "y", make_num(ev.wheel.y));
            break;
        case MY_SDL_WINDOWEVENT:
            /* SDL_WINDOWEVENT_RESIZED = 6 */
            if (ev.window.event == 6) {
                dict_set(d, "type", make_str("resize"));
                dict_set(d, "w", make_num(ev.window.data1));
                dict_set(d, "h", make_num(ev.window.data2));
            } else {
                return make_null();
            }
            break;
        default:
            return make_null();
    }
    return d;
}

/* gfx_ticks of null — milliseconds since SDL_Init */
Value* builtin_gfx_ticks(Value *arg) {
    (void)arg;
    return make_num(g_sdl_lib ? (double)p_SDL_GetTicks() : 0.0);
}

/* gfx_delay of ms */
Value* builtin_gfx_delay(Value *arg) {
    if (!arg || arg->type != VAL_NUM) return make_null();
    if (g_sdl_lib) p_SDL_Delay((Uint32)arg->data.num);
    return make_null();
}

/* gfx_title of "new title" */
Value* builtin_gfx_title(Value *arg) {
    if (!g_window || !arg || arg->type != VAL_STR) return make_null();
    p_SDL_SetWindowTitle(g_window, arg->data.str);
    return make_null();
}

/* 5x7 bitmap font — printable ASCII 32..126 */
static const unsigned char font5x7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* '!' */
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, /* '"' */
    {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /* '#' */
    {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* '$' */
    {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* '%' */
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /* '&' */
    {0x04,0x04,0x08,0x00,0x00,0x00,0x00}, /* ''' */
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* '(' */
    {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ')' */
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* '*' */
    {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* '+' */
    {0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* ',' */
    {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* '-' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x04}, /* '.' */
    {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, /* '/' */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* '0' */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* '1' */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* '2' */
    {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* '3' */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* '4' */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* '5' */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* '6' */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* '7' */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* '8' */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* '9' */
    {0x00,0x00,0x04,0x00,0x04,0x00,0x00}, /* ':' */
    {0x00,0x00,0x04,0x00,0x04,0x04,0x08}, /* ';' */
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* '<' */
    {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* '=' */
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* '>' */
    {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* '?' */
    {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E}, /* '@' */
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* 'A' */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* 'B' */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* 'C' */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* 'D' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* 'E' */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* 'F' */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* 'G' */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* 'H' */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* 'I' */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* 'J' */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* 'K' */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* 'L' */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* 'M' */
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, /* 'N' */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 'O' */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* 'P' */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* 'Q' */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* 'R' */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* 'S' */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* 'T' */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* 'U' */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* 'V' */
    {0x11,0x11,0x11,0x15,0x15,0x1B,0x11}, /* 'W' */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* 'X' */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* 'Y' */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* 'Z' */
    {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* '[' */
    {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, /* '\' */
    {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ']' */
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* '^' */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* '_' */
    {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, /* '`' */
    {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /* 'a' */
    {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}, /* 'b' */
    {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, /* 'c' */
    {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}, /* 'd' */
    {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* 'e' */
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, /* 'f' */
    {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, /* 'g' */
    {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, /* 'h' */
    {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* 'i' */
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* 'j' */
    {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* 'k' */
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* 'l' */
    {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, /* 'm' */
    {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, /* 'n' */
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* 'o' */
    {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, /* 'p' */
    {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}, /* 'q' */
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* 'r' */
    {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}, /* 's' */
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, /* 't' */
    {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, /* 'u' */
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* 'v' */
    {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* 'w' */
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* 'x' */
    {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* 'y' */
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* 'z' */
    {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, /* '{' */
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* '|' */
    {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, /* '}' */
    {0x00,0x04,0x02,0x1F,0x02,0x04,0x00}, /* '~' */
};

/* gfx_text of [x, y, text, r, g, b] or [x, y, text, r, g, b, scale] */
Value* builtin_gfx_text(Value *arg) {
    if (!g_renderer || !arg || arg->type != VAL_LIST || arg->data.list.count < 6) return make_null();
    int x = (int)arg->data.list.items[0]->data.num;
    int y = (int)arg->data.list.items[1]->data.num;
    const char *text = arg->data.list.items[2]->type == VAL_STR ? arg->data.list.items[2]->data.str : "";
    int r = (int)arg->data.list.items[3]->data.num;
    int g = (int)arg->data.list.items[4]->data.num;
    int b = (int)arg->data.list.items[5]->data.num;
    int scale = (arg->data.list.count >= 7) ? (int)arg->data.list.items[6]->data.num : 1;
    if (scale < 1) scale = 1;

    p_SDL_SetRenderDrawColor(g_renderer, r, g, b, 255);
    int cx = x;
    for (const char *p = text; *p; p++) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *glyph = font5x7[ch - 32];
        for (int row = 0; row < 7; row++) {
            unsigned char bits = glyph[row];
            for (int col = 0; col < 5; col++) {
                if (bits & (0x10 >> col)) {
                    if (scale == 1) {
                        p_SDL_RenderDrawPoint(g_renderer, cx + col, y + row);
                    } else {
                        SDL_Rect pr = { cx + col * scale, y + row * scale, scale, scale };
                        p_SDL_RenderFillRect(g_renderer, &pr);
                    }
                }
            }
        }
        cx += (5 + 1) * scale; /* 5 pixel width + 1 pixel gap */
    }
    return make_null();
}

/* ---- Audio Builtins ---- */

/* audio_open of [freq, channels] — open audio device with queue mode */
Value* builtin_audio_open(Value *arg) {
    if (!g_sdl_lib) { if (!load_sdl2()) return make_num(0); }
    if (!p_SDL_OpenAudioDevice) {
        fprintf(stderr, "audio_open: SDL2 audio symbols not available\n");
        return make_num(0);
    }
    p_SDL_Init(MY_SDL_INIT_AUDIO);

    SDL_AudioSpec want = {0}, have = {0};
    want.freq = 44100;
    want.format = MY_SDL_AUDIO_S16LSB;
    want.channels = 1;
    want.samples = 1024;
    want.callback = NULL;

    if (arg && arg->type == VAL_LIST && arg->data.list.count >= 2) {
        want.freq = (int)arg->data.list.items[0]->data.num;
        want.channels = (int)arg->data.list.items[1]->data.num;
    }

    g_audio_device = p_SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_device < 2) { g_audio_device = 0; return make_num(0); }
    g_audio_freq = have.freq;
    g_audio_channels = have.channels;
    p_SDL_PauseAudioDevice(g_audio_device, 0); /* start playing */
    return make_num(g_audio_device);
}

/* audio_close of null */
Value* builtin_audio_close(Value *arg) {
    (void)arg;
    if (g_audio_device) {
        p_SDL_CloseAudioDevice(g_audio_device);
        g_audio_device = 0;
    }
    return make_null();
}

/* audio_pause of flag — 1=pause, 0=unpause */
Value* builtin_audio_pause(Value *arg) {
    if (!g_audio_device) return make_null();
    int pause = (arg && arg->type == VAL_NUM) ? (int)arg->data.num : 1;
    p_SDL_PauseAudioDevice(g_audio_device, pause);
    return make_null();
}

/* audio_play of samples — convert float list [-1,1] to int16, queue */
Value* builtin_audio_play(Value *arg) {
    if (!g_audio_device || !arg || arg->type != VAL_LIST) return make_null();
    int n = arg->data.list.count;
    if (n == 0) return make_null();
    int16_t *buf = xmalloc_array(n, sizeof(int16_t));
    for (int i = 0; i < n; i++) {
        double s = (arg->data.list.items[i]->type == VAL_NUM) ? arg->data.list.items[i]->data.num : 0;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        buf[i] = (int16_t)(s * 32767);
    }
    p_SDL_QueueAudio(g_audio_device, buf, n * sizeof(int16_t));
    free(buf);
    return make_null();
}

/* audio_queue_size of null — bytes queued */
Value* builtin_audio_queue_size(Value *arg) {
    (void)arg;
    if (!g_audio_device) return make_num(0);
    return make_num(p_SDL_GetQueuedAudioSize(g_audio_device));
}

/* audio_clear of null — clear audio queue */
Value* builtin_audio_clear(Value *arg) {
    (void)arg;
    if (g_audio_device) p_SDL_ClearQueuedAudio(g_audio_device);
    return make_null();
}

/* audio_sine of [freq, duration, amplitude] — generate sine wave samples */
Value* builtin_audio_sine(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 3) return make_list(0);
    double freq = arg->data.list.items[0]->data.num;
    double dur = arg->data.list.items[1]->data.num;
    double amp = arg->data.list.items[2]->data.num;
    int rate = g_audio_freq > 0 ? g_audio_freq : 44100;
    int n = (int)(dur * rate);
    if (n <= 0 || n > rate * 30) return make_list(0);

    Value *list = make_list(n);
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double s = sin(2.0 * M_PI * freq * t) * amp;
        list_append(list, make_num(s));
    }
    return list;
}

/* audio_saw of [freq, duration, amplitude] — sawtooth wave */
Value* builtin_audio_saw(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 3) return make_list(0);
    double freq = arg->data.list.items[0]->data.num;
    double dur = arg->data.list.items[1]->data.num;
    double amp = arg->data.list.items[2]->data.num;
    int rate = g_audio_freq > 0 ? g_audio_freq : 44100;
    int n = (int)(dur * rate);
    if (n <= 0 || n > rate * 30) return make_list(0);

    Value *list = make_list(n);
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double s = 2.0 * (t * freq - floor(t * freq + 0.5)) * amp;
        list_append(list, make_num(s));
    }
    return list;
}

/* audio_square of [freq, duration, amplitude] — square wave */
Value* builtin_audio_square(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 3) return make_list(0);
    double freq = arg->data.list.items[0]->data.num;
    double dur = arg->data.list.items[1]->data.num;
    double amp = arg->data.list.items[2]->data.num;
    int rate = g_audio_freq > 0 ? g_audio_freq : 44100;
    int n = (int)(dur * rate);
    if (n <= 0 || n > rate * 30) return make_list(0);

    Value *list = make_list(n);
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        double s = (sin(2.0 * M_PI * freq * t) >= 0 ? 1.0 : -1.0) * amp;
        list_append(list, make_num(s));
    }
    return list;
}

/* audio_noise of [duration, amplitude] — white noise */
Value* builtin_audio_noise(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 2) return make_list(0);
    double dur = arg->data.list.items[0]->data.num;
    double amp = arg->data.list.items[1]->data.num;
    int rate = g_audio_freq > 0 ? g_audio_freq : 44100;
    int n = (int)(dur * rate);
    if (n <= 0 || n > rate * 30) return make_list(0);

    Value *list = make_list(n);
    for (int i = 0; i < n; i++) {
        double s = ((double)rand() / RAND_MAX * 2.0 - 1.0) * amp;
        list_append(list, make_num(s));
    }
    return list;
}

/* audio_mix of [samples_a, samples_b] — add and clamp */
Value* builtin_audio_mix(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 2) return make_list(0);
    Value *a = arg->data.list.items[0];
    Value *b = arg->data.list.items[1];
    if (a->type != VAL_LIST || b->type != VAL_LIST) return make_list(0);
    int n = a->data.list.count > b->data.list.count ? a->data.list.count : b->data.list.count;

    Value *out = make_list(n);
    for (int i = 0; i < n; i++) {
        double sa = (i < a->data.list.count && a->data.list.items[i]->type == VAL_NUM) ? a->data.list.items[i]->data.num : 0;
        double sb = (i < b->data.list.count && b->data.list.items[i]->type == VAL_NUM) ? b->data.list.items[i]->data.num : 0;
        double mixed = sa + sb;
        if (mixed > 1.0) mixed = 1.0;
        if (mixed < -1.0) mixed = -1.0;
        list_append(out, make_num(mixed));
    }
    return out;
}

/* audio_gain of [samples, volume] — scale and clamp */
Value* builtin_audio_gain(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 2) return make_list(0);
    Value *samples = arg->data.list.items[0];
    if (samples->type != VAL_LIST) return make_list(0);
    double vol = arg->data.list.items[1]->data.num;
    int n = samples->data.list.count;

    Value *out = make_list(n);
    for (int i = 0; i < n; i++) {
        double s = (samples->data.list.items[i]->type == VAL_NUM) ? samples->data.list.items[i]->data.num : 0;
        s *= vol;
        if (s > 1.0) s = 1.0;
        if (s < -1.0) s = -1.0;
        list_append(out, make_num(s));
    }
    return out;
}

/* audio_envelope of [samples, attack, decay, sustain_level, release] — ADSR */
Value* builtin_audio_envelope(Value *arg) {
    if (!arg || arg->type != VAL_LIST || arg->data.list.count < 5) return make_list(0);
    Value *samples = arg->data.list.items[0];
    if (samples->type != VAL_LIST) return make_list(0);
    double attack = arg->data.list.items[1]->data.num;
    double decay = arg->data.list.items[2]->data.num;
    double sustain = arg->data.list.items[3]->data.num;
    double release = arg->data.list.items[4]->data.num;

    int n = samples->data.list.count;
    int rate = g_audio_freq > 0 ? g_audio_freq : 44100;
    int a_samples = (int)(attack * rate);
    int d_samples = (int)(decay * rate);
    int r_samples = (int)(release * rate);
    int s_start = a_samples + d_samples;
    int r_start = n - r_samples;
    if (r_start < s_start) r_start = s_start;

    Value *out = make_list(n);
    for (int i = 0; i < n; i++) {
        double env;
        if (i < a_samples) {
            /* Attack: 0 -> 1 */
            env = (a_samples > 0) ? (double)i / a_samples : 1.0;
        } else if (i < s_start) {
            /* Decay: 1 -> sustain */
            double frac = (d_samples > 0) ? (double)(i - a_samples) / d_samples : 1.0;
            env = 1.0 - (1.0 - sustain) * frac;
        } else if (i < r_start) {
            /* Sustain */
            env = sustain;
        } else {
            /* Release: sustain -> 0 */
            double frac = (r_samples > 0) ? (double)(i - r_start) / r_samples : 1.0;
            env = sustain * (1.0 - frac);
        }
        double s = (samples->data.list.items[i]->type == VAL_NUM) ? samples->data.list.items[i]->data.num : 0;
        s *= env;
        list_append(out, make_num(s));
    }
    return out;
}

void register_gfx_builtins(Env *env) {
    env_set_local(env, "gfx_open", make_builtin(builtin_gfx_open));
    env_set_local(env, "gfx_close", make_builtin(builtin_gfx_close));
    env_set_local(env, "gfx_clear", make_builtin(builtin_gfx_clear));
    env_set_local(env, "gfx_rect", make_builtin(builtin_gfx_rect));
    env_set_local(env, "gfx_line", make_builtin(builtin_gfx_line));
    env_set_local(env, "gfx_point", make_builtin(builtin_gfx_point));
    env_set_local(env, "gfx_circle", make_builtin(builtin_gfx_circle));
    env_set_local(env, "gfx_rrect", make_builtin(builtin_gfx_rrect));
    env_set_local(env, "gfx_clip", make_builtin(builtin_gfx_clip));
    env_set_local(env, "gfx_present", make_builtin(builtin_gfx_present));
    env_set_local(env, "gfx_poll", make_builtin(builtin_gfx_poll));
    env_set_local(env, "gfx_ticks", make_builtin(builtin_gfx_ticks));
    env_set_local(env, "gfx_delay", make_builtin(builtin_gfx_delay));
    env_set_local(env, "gfx_title", make_builtin(builtin_gfx_title));
    env_set_local(env, "gfx_text", make_builtin(builtin_gfx_text));

    /* Audio builtins */
    env_set_local(env, "audio_open", make_builtin(builtin_audio_open));
    env_set_local(env, "audio_close", make_builtin(builtin_audio_close));
    env_set_local(env, "audio_pause", make_builtin(builtin_audio_pause));
    env_set_local(env, "audio_play", make_builtin(builtin_audio_play));
    env_set_local(env, "audio_queue_size", make_builtin(builtin_audio_queue_size));
    env_set_local(env, "audio_clear", make_builtin(builtin_audio_clear));
    env_set_local(env, "audio_sine", make_builtin(builtin_audio_sine));
    env_set_local(env, "audio_saw", make_builtin(builtin_audio_saw));
    env_set_local(env, "audio_square", make_builtin(builtin_audio_square));
    env_set_local(env, "audio_noise", make_builtin(builtin_audio_noise));
    env_set_local(env, "audio_mix", make_builtin(builtin_audio_mix));
    env_set_local(env, "audio_gain", make_builtin(builtin_audio_gain));
    env_set_local(env, "audio_envelope", make_builtin(builtin_audio_envelope));
}

#endif /* EIGENSCRIPT_EXT_GFX */
