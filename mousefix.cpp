// mousefix.cpp - mouse pointer fix ASI for WormsMayhem.exe (32-bit), v1.12
//
// What it fixes (see worms_mouse_notes.md):
//
//  1. 640x480 grid. The game moves its pointer (object at [0x96D188], float x,y at
//     +0x28/+0x2C, range -1..1) in function 0x6920CC by
//          x += delta * [obj+0x34]      ([obj+0x34] = 1/320, set once in ctor 0x691B2E)
//          y -= delta * [obj+0x38]      ([obj+0x38] = 1/240)
//     so 1 mouse count always moves the pointer 1/640 of the screen
//     (3 px at 1920, 2 px at 1280).
//     OneToOne=1 (default since v1.12): 1 mouse count moves 1 screen pixel, like the
//     Windows cursor. Slower than the original at high resolutions (3x at 1080p).
//     OneToOne=0: original speed (keeps muscle memory); finer steps then need a higher
//     DPI with Speed lowered by the same factor (Speed = old DPI / new DPI).
//     Speed multiplies either mode.
//
//  2. Backward steps / lost movement (KeepReference=1). 0x70098E (XMouseDeviceWin
//     vtable+0x70) overwrites the "last mouse position" ([dev+0xB0]/[dev+0xB4])
//     with a fresh GetCursorPos; queued WM_MOUSEMOVEs then give wrong deltas.
//     We keep the old value after the first call.
//
//  3. Menu cursor stutter (SmoothCursor=1): the cursor sprite was only moved every
//     20 ms (MouseEntity tick 0x744C0E). We move it every frame.
//
//  4. In-match speed (MatchSensitivity=1): the match pointer (UI manager 0x506570)
//     uses the raw deltas, so Speed is applied there separately.
//
//  5. The game's own mouse sensitivity setting dropped the fraction on every
//     movement (int truncation in 0x506570). Remainders are kept (FixScale=1).
//
//  6. Backward jumps in matches (FixRecenter=1): race between the per-frame
//     recentre (0x701D92) and queued WM_MOUSEMOVEs. Movement is now measured in
//     the recentre step itself; WM_MOUSEMOVE movement is ignored while it runs.
//
// Log=1 writes per-second call rates of the hooked functions to mousefix.log.
// Log=2 also logs every pointer movement with the pointer position (big log - short tests only).
//
// Every patched function's first bytes are checked first; on a different exe
// nothing is patched. Build exactly like mouselog (x86 Release DLL, .asi).
// Settings: mousefix.ini next to mousefix.asi (created with defaults if missing).
// mousefix.log is written next to WormsMayhem.exe, with the game's other logs.


#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#pragma intrinsic(_ReturnAddress)

// ------------------------------------------------------------------ settings
static float g_sens      = 1.0f;   // Speed: 1.0 = exactly the original game speed (menus and matches)
static int   g_fixScale  = 1;
static int   g_oneToOne  = 1;   // 1: Speed 1.0 = 1 mouse count -> 1 screen pixel (ignores original speed)
static int   g_keepRef   = 1;
static int   g_logOn     = 0;
static int   g_smooth    = 1;   // move the menu cursor sprite every frame, not every 20 ms
static int   g_fixRecenter = 1; // in matches: take movement from the recentre step, not WM_MOUSEMOVE
static int   g_matchSens = 1;   // apply Speed to the in-match cursor too (raw-delta path)

// ------------------------------------------------------------------ game addresses (VA)
static const DWORD VA_POINTER_AXIS = 0x6920CC; // thiscall(dev, axis, delta), ret 0xC
static const DWORD VA_MOUSE_RESET  = 0x70098E; // stdcall(dev*, index), ret 8
static const DWORD VA_MOUSE_UPDATE = 0x700524; // stdcall(dev*, out*), ret 8
static const DWORD VA_GAME_HWND    = 0x9668DC; // HWND global (read by 0x6405AC)
static const DWORD VA_UI_MOUSEMSG  = 0x506570; // UI manager, thiscall(msg), ret 4
static const DWORD VA_RECENTER     = 0x701D92; // stdcall(obj, centreX, centreY) screen coords, ret 0xC
static const DWORD VA_PUSH_EVENT   = 0x700B45; // thiscall(dev, event*), ret 4: queue a mouse event
static const DWORD VA_FIND_MOUSE   = 0x701348; // cdecl(x) -> mouse device or 0
static const DWORD VA_MOUSE_TICK   = 0x744C0E; // MouseEntity tick, stdcall(entity*, arg), ret 8

// ------------------------------------------------------------------ logging
static CRITICAL_SECTION g_cs;
static FILE*         g_log = nullptr;
static LARGE_INTEGER g_freq, g_t0;
static volatile LONG g_lines = 0;
static const LONG    MAX_LINES = 60000;
static HMODULE       g_exe = nullptr;
static DWORD         g_exeSize = 0;
static HMODULE       g_self = nullptr;   // this .asi

// Full path of <name> in the folder this .asi is in (plugins/scripts folder).
static void PathNextToAsi(const char* name, char* out)
{
    GetModuleFileNameA(g_self, out, MAX_PATH);
    char* slash = strrchr(out, '\\');
    if (slash) strcpy(slash + 1, name); else strcpy(out, name);
}

static double NowMs()
{
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    if (InterlockedIncrement(&g_lines) > MAX_LINES) return;
    double ms = NowMs();
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "%10.3f T%lu ", ms, (unsigned long)GetCurrentThreadId());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

static unsigned Rva(void* p)
{
    DWORD_PTR a = (DWORD_PTR)p, b = (DWORD_PTR)g_exe;
    return (a >= b && a < b + g_exeSize) ? (unsigned)(a - b) : 0xFFFFFFFFu;
}

// Prints one summary line per second per hook: how many calls, min/max gap,
// and the caller that was seen last. Keeps the log small.
struct RateStat {
    const char* name;
    LONG   calls;
    double last, minGap, maxGap, windowStart;
    unsigned caller;
};
static void RateHit(RateStat& s, void* ra)
{
    if (!g_logOn) return;
    double t = NowMs();
    if (s.calls == 0) { s.windowStart = t; s.minGap = 1e9; s.maxGap = 0; }
    else {
        double g = t - s.last;
        if (g < s.minGap) s.minGap = g;
        if (g > s.maxGap) s.maxGap = g;
    }
    s.last = t;
    s.caller = Rva(ra);
    ++s.calls;
    if (t - s.windowStart >= 1000.0) {
        Log("%-12s %4ld calls in %.0f ms  gap %.1f..%.1f ms  last caller exe+0x%X",
            s.name, s.calls, t - s.windowStart, s.calls > 1 ? s.minGap : 0.0, s.maxGap, s.caller);
        s.calls = 0;
    }
}
static RateStat st_axis   = { "PointerAxis" };
static RateStat st_reset  = { "MouseReset" };
static RateStat st_update = { "MouseUpdate" };
static RateStat st_tick   = { "CursorTick" };

// ------------------------------------------------------------------ detour helper
static bool InstallDetour(DWORD va, const BYTE* expected, int stolen, void* hook, void** origOut)
{
    BYTE* target = (BYTE*)g_exe + (va - 0x400000);
    if (memcmp(target, expected, stolen) != 0) {
        Log("detour 0x%X: bytes differ from expected, NOT installed", (unsigned)va);
        return false;
    }
    BYTE* tramp = (BYTE*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { Log("detour 0x%X: VirtualAlloc failed", (unsigned)va); return false; }
    memcpy(tramp, target, stolen);
    tramp[stolen] = 0xE9;
    *(DWORD*)(tramp + stolen + 1) = (DWORD)(target + stolen) - (DWORD)(tramp + stolen + 5);
    *origOut = tramp;

    DWORD old;
    VirtualProtect(target, stolen, PAGE_EXECUTE_READWRITE, &old);
    target[0] = 0xE9;
    *(DWORD*)(target + 1) = (DWORD)hook - (DWORD)(target + 5);
    for (int i = 5; i < stolen; ++i) target[i] = 0x90;
    VirtualProtect(target, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, stolen);
    Log("detour installed at 0x%X", (unsigned)va);
    return true;
}

// Width/height of the game window (falls back to 640x480 = original behaviour).
static void GameSize(double& w, double& h)
{
    w = 640.0; h = 480.0;
    HWND hw = *(HWND*)VA_GAME_HWND;
    RECT rc;
    if (hw && GetClientRect(hw, &rc) && rc.right > 0 && rc.bottom > 0) { w = rc.right; h = rc.bottom; }
}

// How much faster than the original game the pointer moves (1.0 = original).
static double SpeedRatio()
{
    if (!g_oneToOne) return (double)g_sens;
    double w, h; GameSize(w, h);
    return (double)g_sens * 640.0 / w;           // 1 count = Speed screen pixels
}

// ------------------------------------------------------------------ fix 1: pointer step size
// 0x6920CC: [obj+0x28] x, [obj+0x2C] y, [obj+0x30] wheel, [obj+0x34] x step, [obj+0x38] y step
typedef void (__fastcall *PointerAxis_t)(void* self, void* edx, int dev, int axis, int delta);
static PointerAxis_t orig_PointerAxis = nullptr;

static void __fastcall Hook_PointerAxis(void* self, void* edx, int dev, int axis, int delta)
{
    RateHit(st_axis, _ReturnAddress());
    if (g_fixScale && self) {
        // original: 1/320 and 1/240 per mouse count. Speed scales that, so 1.0 = untouched.
        // OneToOne=1: Speed screen pixels per count (2/width, 2/height of the -1..1 range).
        if (g_oneToOne) {
            double w, h; GameSize(w, h);
            *(float*)((BYTE*)self + 0x34) = (float)(g_sens * 2.0 / w);
            *(float*)((BYTE*)self + 0x38) = (float)(g_sens * 2.0 / h);
        } else {
            *(float*)((BYTE*)self + 0x34) = g_sens / 320.0f;
            *(float*)((BYTE*)self + 0x38) = g_sens / 240.0f;
        }
    }
    float bx = 0, by = 0;
    if (g_logOn >= 2 && self) { bx = *(float*)((BYTE*)self + 0x28); by = *(float*)((BYTE*)self + 0x2C); }
    orig_PointerAxis(self, edx, dev, axis, delta);
    if (g_logOn >= 2 && self && axis <= 1) {
        // Log=2: every movement with the pointer position before/after (range -1..1).
        // Shows clamping at the edges and resets done elsewhere (before != last after).
        float ax = *(float*)((BYTE*)self + 0x28), ay = *(float*)((BYTE*)self + 0x2C);
        Log("Axis %c %+4d  pos x %.4f -> %.4f  y %.4f -> %.4f", axis == 0 ? 'x' : 'y', delta,
            (double)bx, (double)ax, (double)by, (double)ay);
    }
}

// ------------------------------------------------------------------ fix 2: keep last mouse position
typedef HRESULT (__stdcall *MouseReset_t)(void* dev, int index);
static MouseReset_t orig_MouseReset = nullptr;
static void* g_resetDev = nullptr;   // device we have already seen once

static HRESULT __stdcall Hook_MouseReset(void* dev, int index)
{
    RateHit(st_reset, _ReturnAddress());
    BYTE* d = (BYTE*)dev;
    int oldX = *(int*)(d + 0xB0), oldY = *(int*)(d + 0xB4);
    HRESULT r = orig_MouseReset(dev, index);
    if (g_keepRef) {
        if (g_resetDev == dev) {           // not the first call: undo the overwrite
            *(int*)(d + 0xB0) = oldX;
            *(int*)(d + 0xB4) = oldY;
        } else {
            g_resetDev = dev;              // first call: let the game take a real position
        }
    }
    return r;
}

// ------------------------------------------------------------------ diagnostics only
typedef HRESULT (__stdcall *MouseUpdate_t)(void* dev, void* out);
static MouseUpdate_t orig_MouseUpdate = nullptr;

static HRESULT __stdcall Hook_MouseUpdate(void* dev, void* out)
{
    RateHit(st_update, _ReturnAddress());
    return orig_MouseUpdate(dev, out);
}

// ------------------------------------------------------------------ fix 3: cursor sprite every frame
// 0x744C0E (MouseEntity tick) only does work once 20 ms have passed: then it copies
// the cursor position (float x,y,z at +0x28) minus a (-14, 14, 0) offset to the
// sprite object at [+0x24] via its vtable+0x50 (stdcall sprite, vec3*, 0), and steps
// the cursor animation. So the drawn cursor moves at most 50 times a second.
// We let the original run (animation timing unchanged), then set the sprite
// position again on every call.
struct Vec3 { float x, y, z; };
typedef void    (__stdcall *SetPos_t)(void* sprite, Vec3* pos, int flag);
typedef HRESULT (__stdcall *MouseTick_t)(void* ent, int arg);
static MouseTick_t orig_MouseTick = nullptr;

static HRESULT __stdcall Hook_MouseTick(void* ent, int arg)
{
    RateHit(st_tick, _ReturnAddress());
    HRESULT r = orig_MouseTick(ent, arg);
    if (g_smooth && ent) {
        BYTE* e = (BYTE*)ent;
        void* sprite = *(void**)(e + 0x24);
        if (sprite) {
            Vec3 v = { *(float*)(e + 0x28) + 14.0f, *(float*)(e + 0x2C) - 14.0f, *(float*)(e + 0x30) };
            SetPos_t setPos = (SetPos_t)(*(void***)sprite)[0x50 / 4];
            setPos(sprite, &v, 0);
        }
    }
    return r;
}

// ------------------------------------------------------------------ fix 4: in-match speed
// In a match the UI manager (0x506570) moves its own pointer by the RAW pixel deltas in
// each 0x1004 message (short dx at msg+0x10, dy at msg+0x12, type dword msg+0x08 = 0),
// not by the float position, so Speed did not reach it and a higher DPI made it
// faster. We scale those deltas by the same factor the menu pointer got:
//     ratio = Speed   (1.0 = original speed, message left untouched)
// keeping the rounding remainder so slow movement is not lost, and put the original
// values back afterwards so other listeners see the message unchanged.
typedef DWORD (__fastcall *UiMouseMsg_t)(void* self, void* edx, void* msg);
static UiMouseMsg_t orig_UiMouseMsg = nullptr;
static double g_carryX = 0.0, g_carryY = 0.0;

static short ScaleDelta(short d, double ratio, double& carry)
{
    double v = d * ratio + carry;
    double r = (v >= 0) ? (double)(long)(v + 0.5) : -(double)(long)(-v + 0.5);
    carry = v - r;
    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
    return (short)r;
}

static DWORD __fastcall Hook_UiMouseMsg(void* self, void* edx, void* msg)
{
    BYTE* m = (BYTE*)msg;
    BYTE* mgr = (BYTE*)self;
    if (!g_fixScale || !m || !mgr || *(DWORD*)(m + 0x08) != 0)
        return orig_UiMouseMsg(self, edx, msg);

    // The game does pos += (int)(delta * [mgr+0xA4]) per message ([mgr+0xA4] = the
    // game's own sensitivity setting), throwing the fraction away. We do the whole
    // multiplication here (game sensitivity * our Speed), keep the remainder for the
    // next message, and let the game add the rounded result with its scale set to 1.
    float  gameScale = *(float*)(mgr + 0xA4);
    double ratio = (double)gameScale * (g_matchSens ? SpeedRatio() : 1.0);
    if (ratio == 1.0) return orig_UiMouseMsg(self, edx, msg);

    short ox = *(short*)(m + 0x10), oy = *(short*)(m + 0x12);
    if (ox) *(short*)(m + 0x10) = ScaleDelta(ox, ratio, g_carryX);
    if (oy) *(short*)(m + 0x12) = ScaleDelta(oy, ratio, g_carryY);
    *(float*)(mgr + 0xA4) = 1.0f;
    DWORD r = orig_UiMouseMsg(self, edx, msg);
    *(float*)(mgr + 0xA4) = gameScale;
    *(short*)(m + 0x10) = ox;
    *(short*)(m + 0x12) = oy;
    return r;
}

// ------------------------------------------------------------------ fix 6: backward jumps in matches
// In a match 0x701D92 runs every frame: if the cursor left the window centre it
// adds (centre - cursor) to the last-known position [dev+0xB0]/[dev+0xB4] and
// SetCursorPos(centre). Movement itself is measured in WM_MOUSEMOVE as
// lParam - last-known. A WM_MOUSEMOVE that was already queued before the recentre
// is then measured against the shifted value: it reports too much, and the
// WM_MOUSEMOVE caused by SetCursorPos reports the difference BACKWARDS. The sum is
// right but the view overshoots and jumps back.
// Fix: while recentring is active we measure the movement right here
// (GetCursorPos - centre, which keeps Windows' pointer speed exactly as before),
// queue it ourselves, and drop the movement events from WM_MOUSEMOVE.
struct MouseEvent { BYTE kind; BYTE pad[3]; int axis; int value; };   // kind 0 = axis
typedef void  (__fastcall *PushEvent_t)(void* dev, void* edx, MouseEvent* ev);
typedef HRESULT (__stdcall *Recenter_t)(void* obj, int cx, int cy);
typedef void* (__stdcall *GetSub_t)(void* obj, int which);
typedef void* (__cdecl   *FindMouse_t)(void* x);
static PushEvent_t orig_PushEvent = nullptr;
static Recenter_t  orig_Recenter  = nullptr;
static volatile DWORD g_lastRecenter = 0;
static volatile bool  g_recSeen = false;   // a recentre has happened at least once
static bool g_ownPush = false;
static RateStat st_recenter = { "Recenter" };
// per-second movement bookkeeping (Log=1): what WM_MOUSEMOVE reported vs what we sent
static struct { DWORD t0; long wmX, wmY, dropX, dropY, ownX, ownY, devNull, recCalls;
                long posX, negX, posY, negY, negXev; } g_mv;   // what actually reaches the game, by direction
static void MoveStatsTick()
{
    if (!g_logOn) return;
    DWORD now = GetTickCount();
    if (!g_mv.t0) g_mv.t0 = now;
    if (now - g_mv.t0 >= 1000) {
        if (g_mv.wmX || g_mv.wmY || g_mv.ownX || g_mv.ownY || g_mv.dropX || g_mv.dropY)
            Log("Moves/s  WM_MOUSEMOVE x=%ld y=%ld (dropped x=%ld y=%ld)  ours x=%ld y=%ld  recentres=%ld noDevice=%ld"
                "  | to game: right=%ld left=%ld (left events=%ld) down=%ld up=%ld",
                g_mv.wmX, g_mv.wmY, g_mv.dropX, g_mv.dropY, g_mv.ownX, g_mv.ownY, g_mv.recCalls, g_mv.devNull,
                g_mv.posX, g_mv.negX, g_mv.negXev, g_mv.posY, g_mv.negY);
        memset(&g_mv, 0, sizeof g_mv);
        g_mv.t0 = now;
    }
}

static void __fastcall Hook_PushEvent(void* dev, void* edx, MouseEvent* ev)
{
    bool move = ev && ev->kind == 0 && (ev->axis == 0 || ev->axis == 1);
    if (move && !g_ownPush) {
        if (ev->axis == 0) g_mv.wmX += ev->value; else g_mv.wmY += ev->value;
        if (g_fixRecenter && g_recSeen && (DWORD)(GetTickCount() - g_lastRecenter) < 200) {
            if (g_logOn >= 2) Log("Drop WM  %c %+d", ev->axis == 0 ? 'x' : 'y', ev->value);
            if (ev->axis == 0) g_mv.dropX += ev->value; else g_mv.dropY += ev->value;
            MoveStatsTick();
            return;                               // WM_MOUSEMOVE delta while recentring: drop
        }
    }
    if (move && g_logOn >= 2)
        Log("Push %s %c %+d", g_ownPush ? "own" : "WM ", ev->axis == 0 ? 'x' : 'y', ev->value);
    if (move) {                                   // this event goes to the game
        if (ev->axis == 0) { if (ev->value > 0) g_mv.posX += ev->value; else { g_mv.negX -= ev->value; ++g_mv.negXev; } }
        else               { if (ev->value > 0) g_mv.posY += ev->value; else g_mv.negY -= ev->value; }
    }
    MoveStatsTick();
    orig_PushEvent(dev, edx, ev);
}

static HRESULT __stdcall Hook_Recenter(void* obj, int cx, int cy)
{
    RateHit(st_recenter, _ReturnAddress());
    ++g_mv.recCalls;
    MoveStatsTick();
    if (!g_fixRecenter || !obj) return orig_Recenter(obj, cx, cy);
    g_lastRecenter = GetTickCount();   // (v1.10 and older used |1 here: half the time that was
    g_recSeen = true;                  //  1 ms in the future, the check wrapped and WM deltas leaked)

    POINT p;
    if (!GetCursorPos(&p) || (p.x == cx && p.y == cy)) return 0;
    if (g_logOn >= 2) Log("Recenter cursor=(%ld,%ld) centre=(%d,%d)", p.x, p.y, cx, cy);

    void* sub = ((GetSub_t)(*(void***)obj)[0x3C / 4])(obj, 1);     // same as the original
    void* dev = ((FindMouse_t)VA_FIND_MOUSE)(sub);
    SetCursorPos(cx, cy);
    if (!dev) ++g_mv.devNull;
    if (dev) {
        int dx = p.x - cx, dy = p.y - cy;
        g_mv.ownX += dx; g_mv.ownY += dy;
        g_ownPush = true;
        if (dx) { MouseEvent e = { 0, {0,0,0}, 0, dx }; Hook_PushEvent(dev, nullptr, &e); }
        if (dy) { MouseEvent e = { 0, {0,0,0}, 1, dy }; Hook_PushEvent(dev, nullptr, &e); }
        g_ownPush = false;
    }
    return 0;
}

// ------------------------------------------------------------------ setup
static void LoadIni()
{
    char path[MAX_PATH];
    PathNextToAsi("mousefix.ini", path);

    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        // First run: write a commented default ini (same text as the shipped mousefix.ini).
        static const char kDefaultIni[] =
        "; mousefix.ini - mouse fix for Worms Ultimate Mayhem\r\n"
        "; Changes take effect after restarting the game.\r\n"
        "\r\n"
        "[MouseFix]\r\n"
        "; OneToOne=1: the cursor moves exactly like your Windows mouse (1 mouse step = 1 pixel).\r\n"
        ";   Menus feel like Windows; in matches the game's own mouse sensitivity is applied on top.\r\n"
        ";   NOTE: this is slower than the original game at high resolutions (3x slower at 1080p),\r\n"
        ";   because the original moved in 640x480 steps. Raise the in-game sensitivity for faster matches.\r\n"
        "; OneToOne=0: original game speed (keeps old muscle memory, but coarse 640x480 steps).\r\n"
        "OneToOne=1\r\n"
        "\r\n"
        "; Speed multiplier on top of the above. 1.0 = no change.\r\n"
        "Speed=1.0\r\n"
        "\r\n"
        "; Fixes (1 = on, 0 = off)\r\n"
        "FixScale=1          ; master switch for the speed/precision fixes\r\n"
        "KeepReference=1     ; no lost/backward steps when the game re-reads the cursor\r\n"
        "SmoothCursor=1      ; menu cursor drawn every frame instead of every 20 ms\r\n"
        "MatchSensitivity=1  ; apply Speed/OneToOne to the in-match cursor too\r\n"
        "FixRecenter=1       ; no backward jumps while aiming in matches\r\n"
        "\r\n"
        "; Diagnostics: 0 = off, 1 = summary log, 2 = every mouse movement (big file, tests only).\r\n"
        "; The log is written to mousefix.log in the game folder.\r\n"
        "Log=0\r\n";
        FILE* f = fopen(path, "wb");
        if (f) { fwrite(kDefaultIni, 1, sizeof kDefaultIni - 1, f); fclose(f); }
    }
    char buf[64];
    GetPrivateProfileStringA("MouseFix", "Speed", "1.0", buf, sizeof buf, path);
    g_sens = (float)atof(buf);
    if (g_sens <= 0.0f || g_sens > 20.0f) g_sens = 1.0f;
    g_fixScale = GetPrivateProfileIntA("MouseFix", "FixScale", 1, path);
    g_oneToOne = GetPrivateProfileIntA("MouseFix", "OneToOne", 1, path);
    g_keepRef  = GetPrivateProfileIntA("MouseFix", "KeepReference", 1, path);
    g_logOn    = GetPrivateProfileIntA("MouseFix", "Log", 0, path);
    g_smooth   = GetPrivateProfileIntA("MouseFix", "SmoothCursor", 1, path);
    g_matchSens = GetPrivateProfileIntA("MouseFix", "MatchSensitivity", 1, path);
    g_fixRecenter = GetPrivateProfileIntA("MouseFix", "FixRecenter", 1, path);
}

static DWORD WINAPI InitThread(LPVOID)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    InitializeCriticalSection(&g_cs);

    g_exe = GetModuleHandleA(nullptr);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)g_exe + ((IMAGE_DOS_HEADER*)g_exe)->e_lfanew);
    g_exeSize = nt->OptionalHeader.SizeOfImage;

    LoadIni();
    if (g_logOn) {
        char path[MAX_PATH];
        GetModuleFileNameA(nullptr, path, MAX_PATH);          // log goes in the game folder
        char* slash = strrchr(path, '\\');
        if (slash) strcpy(slash + 1, "mousefix.log");
        g_log = fopen(path, "w");
    }
    Log("mousefix v1.12. Speed=%.3f OneToOne=%d FixScale=%d KeepReference=%d SmoothCursor=%d MatchSensitivity=%d FixRecenter=%d",
        (double)g_sens, g_oneToOne, g_fixScale, g_keepRef, g_smooth, g_matchSens, g_fixRecenter);

    static const BYTE exp_6920CC[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x74 };  // push ebp; mov ebp,esp; sub esp,74h
    static const BYTE exp_70098E[] = { 0x55, 0x8B, 0xEC, 0x51, 0x51 };        // push ebp; mov ebp,esp; push ecx; push ecx
    static const BYTE exp_700524[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x58 };  // push ebp; mov ebp,esp; sub esp,58h
    InstallDetour(VA_POINTER_AXIS, exp_6920CC, 6, (void*)Hook_PointerAxis, (void**)&orig_PointerAxis);
    InstallDetour(VA_MOUSE_RESET,  exp_70098E, 5, (void*)Hook_MouseReset,  (void**)&orig_MouseReset);
    static const BYTE exp_744C0E[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C };  // push ebp; mov ebp,esp; sub esp,0Ch
    static const BYTE exp_506570[] = { 0x83, 0xEC, 0x2C, 0x55, 0x8B, 0x6C, 0x24, 0x34 }; // sub esp,2Ch; push ebp; mov ebp,[esp+34h]
    InstallDetour(VA_UI_MOUSEMSG, exp_506570, 8, (void*)Hook_UiMouseMsg, (void**)&orig_UiMouseMsg);
    // push esi; mov esi,ecx; push edi; lea edi,[esi+0ACh]
    static const BYTE exp_700B45[] = { 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0xBE, 0xAC, 0x00, 0x00, 0x00 };
    static const BYTE exp_701D92[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 };  // push ebp; mov ebp,esp; sub esp,10h
    if (InstallDetour(VA_PUSH_EVENT, exp_700B45, 10, (void*)Hook_PushEvent, (void**)&orig_PushEvent))
        InstallDetour(VA_RECENTER, exp_701D92, 6, (void*)Hook_Recenter, (void**)&orig_Recenter);
    InstallDetour(VA_MOUSE_TICK, exp_744C0E, 6, (void*)Hook_MouseTick, (void**)&orig_MouseTick);
    if (g_logOn)
        InstallDetour(VA_MOUSE_UPDATE, exp_700524, 6, (void*)Hook_MouseUpdate, (void**)&orig_MouseUpdate);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        // Patch right away (the functions are only called once the game runs);
        // a thread keeps DllMain short, like mouselog.
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
