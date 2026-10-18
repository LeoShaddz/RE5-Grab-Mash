// RE5_GrabMash - Resident Evil 5 (re5dx9.exe, Steam, PC)
// Holding the controller's A button (XInput) or an extra key (ExtraKeyVK in .ini) while the game prompts
// you to shake or rotate the left analog stick (grabs and rotation prompts) makes the game "see" the analog
// stick moving. Outside of these prompts, holding A/key does nothing.
// NOT COMPILED nor TESTED (cannot run the game here). Based on RE5_Probe/RE5_GrabMash logs from your exe.
//
// How it works
//  1) Prompt detection: the game creates a "prompt request" by calling the function at 0xBC2DA0. The 6th argument
//     encodes the prompt: high byte = type (6..9 = analog prompts), low byte = icon
//     (0x14 G_STICK = grab, 0x1A = left/right, 0x1B = up/down, 0x1C and 0x15 = rotate).
//     The mod hooks this function just to read the arguments (it keeps running normally) and stores the created object.
//     The prompt counts as "active" while this object remains alive (same vtable and same type) and for at most MaxPromptMs.
//  2) Input: hooks the XInputGetState function inside the XInput DLL (xinput1_3/1_4/9_1_0), chaining with
//     any hook another program has already made. If there is an active prompt and the CONTROLLER's A is pressed,
//     the mod returns the left analog to the game: shaking left/right (grab, left/right), up/down, or rotating in a circle.
//     Thus the game's own logic counts the movement; the mod doesn't touch any counter.
//  3) Keyboard (no controller): if the extra key (ExtraKeyVK) is pressed and there is an active analog prompt,
//     the mod simulates alternating A/D (W/S for up/down prompts) with SendInput. No controller is needed and
//     it DOES NOT touch controller input (the game continues showing keyboard icons).
//     Use an extra key different from A/D/W/S.
//  4) Speed: side-swapping is synchronized with game frames (counter at obj+0x64): a different side
//     every frame, which is the maximum the game reads (input is only read once per frame).
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

typedef unsigned char  u8;
typedef unsigned int   u32;

static const u32 FN_REQCTOR   = 0xBC2DA0;   // prompt request ctor (thiscall, 19 args, ret 0x4C)
static const u32 REQ_VTABLE   = 0x1037A60;  // request object vtable (seen in log)

static int gEnable = 1, gKeyMs = 60, gPadMs = 60, gPadRotMs = 700, gGraceMs = 700, gAmp = 30000, gPlayer = -1, gMaxPromptMs = 10000, gLog = 0, gVk = 0;
static int gKeyLeft = 'A', gKeyRight = 'D', gKeyUp = 'W', gKeyDown = 'S';

static char  gDir[MAX_PATH] = "";
static FILE* gF = NULL;
static void Log(const char* f, ...) {
    if (!gLog) return;
    if (!gF) { char p[MAX_PATH]; strcpy(p, gDir); strcat(p, "RE5_GrabMash.log"); gF = fopen(p, "a"); }
    if (!gF) return;
    va_list a; va_start(a, f); vfprintf(gF, f, a); va_end(a); fputc('\n', gF); fflush(gF);
}

// ---------------- prompt detection ----------------
struct Req { volatile u32 obj, tick, player, glyph, kind; };
static const int NRING = 8;
static Req gRing[NRING];
static volatile LONG gRingPos = 0;

// called by the stub: this = constructor ecx; sp = esp right after pushfd/pushad
static void __cdecl OnReq(u32 thisPtr, u32 sp) {
    const u32* args = (const u32*)(sp + 0x28);              // arg1..arg19
    u32 a6 = args[5];
    u32 kind = (a6 >> 8) & 0xFF;
    if (kind >= 6 && kind <= 9) {
        int i = (int)(InterlockedIncrement(&gRingPos) & (NRING - 1));
        gRing[i].obj = 0;
        gRing[i].tick = GetTickCount(); gRing[i].player = args[2]; gRing[i].glyph = a6 & 0xFF; gRing[i].kind = kind;
        gRing[i].obj = thisPtr;
        Log("analog prompt created: obj=%08X player=%u type=%u icon=%02X (arg6=%X)", thisPtr, args[2], kind, a6 & 0xFF, a6);
    }
}

// 0 = no active prompt; 1 = shake left/right; 2 = up/down; 3 = rotate
static int PromptModeRaw(u32* objOut = NULL) {
    DWORD now = GetTickCount();
    u32 bestTick = 0, bestObj = 0; int mode = 0;
    for (int i = 0; i < NRING; ++i) {
        u32 o = gRing[i].obj;
        if (!o) continue;
        if (now - gRing[i].tick > (DWORD)gMaxPromptMs) continue;
        if (gPlayer >= 0 && gRing[i].player != (u32)gPlayer) continue;
        if (IsBadReadPtr((void*)o, 0x30)) continue;
        const u32* p = (const u32*)o;
        if (p[0] != REQ_VTABLE) continue;               // object released/reused
        if (p[6] != gRing[i].kind) continue;            // +0x18: type
        if (gRing[i].tick < bestTick) continue;
        bestTick = gRing[i].tick; bestObj = o;
        u32 g = gRing[i].glyph;
        mode = (g == 0x1C || g == 0x15) ? 3 : (g == 0x1B ? 2 : 1);
    }
    if (objOut) *objOut = mode ? bestObj : 0;
    return mode;
}

static void DumpRing(const char* why) {
    DWORD now = GetTickCount();
    for (int i = 0; i < NRING; ++i) {
        u32 o = gRing[i].obj;
        if (!o) continue;
        if (IsBadReadPtr((void*)o, 0x30)) { Log("  [%s] slot%d obj=%08X unreadable", why, i, o); continue; }
        const u32* p = (const u32*)o;
        u32 f = IsBadReadPtr((void*)(o + 0x64), 4) ? 0 : *(const u32*)(o + 0x64);
        Log("  [%s] slot%d obj=%08X age=%ums vtable=%08X current_type=%u created_type=%u created_icon=%02X current_icon=%u +64=%u",
            why, i, o, (unsigned)(now - gRing[i].tick), p[0], p[6], gRing[i].kind, gRing[i].glyph, p[8], f);
    }
}

// Same as PromptModeRaw, but holds the last mode for GraceMs after the prompt "disappears" (the game might swap/recreate
// the object near the end of a rotation prompt; without this, injection would stop slightly before finishing).
static volatile LONG gHeldMode = 0, gHeldObj = 0, gHeldTime = 0, gLoggedMode = 0;
static int PromptMode(u32* objOut = NULL) {
    u32 obj = 0;
    int m = PromptModeRaw(&obj);
    DWORD now = GetTickCount();
    if (m != gLoggedMode) { gLoggedMode = m; Log("prompt mode now: %d", m); DumpRing(m ? "changed" : "gone"); }
    if (m) {
        gHeldMode = m; gHeldObj = (LONG)obj; gHeldTime = (LONG)now;
    } else if (gHeldMode) {
        if (gGraceMs > 0 && now - (DWORD)gHeldTime < (DWORD)gGraceMs) { m = (int)gHeldMode; obj = (u32)gHeldObj; }
        else { Log("prompt ended (last mode=%d)", (int)gHeldMode); gHeldMode = 0; }
    }
    if (objOut) *objOut = obj;
    return m;
}

// "Phase" synchronized with game frames: the prompt object has a frame counter at +0x64.
// Each time it changes (1 frame) the phase advances by 1; if it doesn't move (rotation prompt, end of count),
// it falls back to a ~16 ms clock. Thus left/right swaps every frame, the maximum the game can read.
static volatile LONG gPhase = 0, gLastV = 0, gLastT = 0, gLastChg = 0;
static u32 Phase(u32 obj) {
    DWORD now = GetTickCount();
    if (obj && !IsBadReadPtr((void*)(obj + 0x64), 4)) {
        LONG v = *(volatile LONG*)(obj + 0x64);
        if (InterlockedExchange(&gLastV, v) != v) { InterlockedIncrement(&gPhase); gLastChg = (LONG)now; }
    }
    if (now - (DWORD)gLastChg > 40) {
        LONG t = (LONG)(now / 16);
        if (InterlockedExchange(&gLastT, t) != t) InterlockedIncrement(&gPhase);
    }
    return (u32)gPhase;
}

static bool InstallReqHook() {
    static const u8 sig[] = { 0x8B, 0x44, 0x24, 0x0C, 0x83, 0xEC, 0x08, 0x55, 0x56, 0x57, 0x8B, 0xF1 };
    if (IsBadReadPtr((void*)FN_REQCTOR, 16)) return false;
    if (memcmp((void*)FN_REQCTOR, sig, sizeof(sig)) != 0) return false;
    u8* st = (u8*)VirtualAlloc(NULL, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!st) return false;
    u8* p = st;
    *p++ = 0x9C;                                // pushfd
    *p++ = 0x60;                                // pushad
    *p++ = 0x8B; *p++ = 0xC4;                   // mov eax, esp
    *p++ = 0x50;                                // push eax (sp)
    *p++ = 0x51;                                // push ecx (this)
    *p++ = 0xE8; *(u32*)p = (u32)&OnReq - (u32)(p + 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;    // add esp, 8
    *p++ = 0x61;                                // popad
    *p++ = 0x9D;                                // popfd
    memcpy(p, (void*)FN_REQCTOR, 7); p += 7;    // mov eax,[esp+0xC] ; sub esp,8
    *p++ = 0xE9; *(u32*)p = (FN_REQCTOR + 7) - (u32)(p + 4); p += 4;
    DWORD old;
    if (!VirtualProtect((void*)FN_REQCTOR, 7, PAGE_EXECUTE_READWRITE, &old)) return false;
    u8* f = (u8*)FN_REQCTOR;
    f[0] = 0xE9; *(u32*)(f + 1) = (u32)st - (FN_REQCTOR + 5);
    f[5] = 0x90; f[6] = 0x90;
    VirtualProtect((void*)FN_REQCTOR, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)FN_REQCTOR, 7);
    return true;
}

// ---------------- XInput input ----------------
struct XPAD { WORD wButtons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
struct XSTATE { DWORD packet; XPAD pad; };
typedef DWORD (WINAPI *XGet_t)(DWORD, XSTATE*);

static const int NX = 3;
static XGet_t gTramp[NX] = { NULL, NULL, NULL };
static bool   gHooked[NX] = { false, false, false };
static volatile LONG gCalls = 0;
static volatile LONG gLastState = -1;

static void Apply(DWORD idx, XSTATE* s) {
    InterlockedIncrement(&gCalls);
    if (!s || !gEnable) return;
    // Only the controller's physical A affects controller input. The extra key DOES NOT pass through here (it uses only
    // the keyboard), so the game doesn't think you are using a controller and switch icons.
    bool a = (s->pad.wButtons & 0x1000) != 0;
    u32 obj = 0;
    int mode = a ? PromptMode(&obj) : 0;
    LONG st = (a ? 1 : 0) | (mode << 1);
    if (st != gLastState) { gLastState = st; Log("pad %u: A=%d mode=%d (calls=%ld)", idx, a ? 1 : 0, mode, gCalls); }
    if (!mode) return;
    (void)obj;
    DWORD t = GetTickCount();       // controller uses time (like v4, which worked): swapping sides every frame is not read as analog
    s->pad.wButtons &= ~0x1000;                         // A stops counting as "pressed"
    if (mode == 3) {                                    // rotate: full circle every PadRotateMs
        double ang = 6.283185307179586 * (double)(t % (DWORD)gPadRotMs) / (double)gPadRotMs;
        s->pad.lx = (SHORT)(cos(ang) * gAmp);
        s->pad.ly = (SHORT)(sin(ang) * gAmp);
    } else {
        SHORT v = (((t / (DWORD)gPadMs) & 1) ? (SHORT)gAmp : (SHORT)-gAmp);
        if (mode == 2) { s->pad.lx = 0; s->pad.ly = v; }              // up/down
        else           { s->pad.lx = v; s->pad.ly = 0; }              // left/right (grab)
    }
}
#define DEFHOOK(i) static DWORD WINAPI HookX##i(DWORD idx, XSTATE* s) { DWORD r = gTramp[i](idx, s); if (r == ERROR_SUCCESS) Apply(idx, s); return r; }
DEFHOOK(0) DEFHOOK(1) DEFHOOK(2)
static void* const gHooks[NX] = { (void*)&HookX0, (void*)&HookX1, (void*)&HookX2 };

// Direct hook on the XInputGetState function inside the DLL itself (catches any caller).
// Accepts standard prologue (mov edi,edi; push ebp; mov ebp,esp) or a "jmp rel32" that another program
// (overlay, another mod) has already placed: in this case the trampoline continues to its destination.
static bool HookFunc(int i, u32 target) {
    static const u8 pro[5] = { 0x8B, 0xFF, 0x55, 0x8B, 0xEC };
    if (IsBadReadPtr((void*)target, 16)) return false;
    const u8* c = (const u8*)target;
    u8* tr = (u8*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tr) return false;
    if (memcmp(c, pro, 5) == 0) {
        memcpy(tr, c, 5);
        tr[5] = 0xE9; *(u32*)(tr + 6) = (target + 5) - (u32)(tr + 10);
    } else if (c[0] == 0xE9) {
        u32 dest = target + 5 + *(const u32*)(c + 1);
        Log("xinput already hooked by another program: %08X -> %08X (chaining)", target, dest);
        tr[0] = 0xE9; *(u32*)(tr + 1) = dest - (u32)(tr + 5);
    } else {
        Log("unexpected prologue at %08X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", target,
            c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9]);
        return false;
    }
    DWORD old;
    if (!VirtualProtect((void*)target, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    gTramp[i] = (XGet_t)tr;
    u8* f = (u8*)target;
    f[0] = 0xE9; *(u32*)(f + 1) = (u32)gHooks[i] - (target + 5);
    VirtualProtect((void*)target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)target, 5);
    return true;
}

static int HookXInput() {
    const char* dlls[NX] = { "xinput1_3.dll", "xinput1_4.dll", "xinput9_1_0.dll" };
    int n = 0;
    for (int i = 0; i < NX; ++i) {
        if (gHooked[i]) { ++n; continue; }
        HMODULE h = GetModuleHandleA(dlls[i]);
        if (!h) continue;
        u32 fn = (u32)GetProcAddress(h, "XInputGetState");
        if (!fn) continue;
        gHooked[i] = true;                                            // only tries once per DLL
        if (HookFunc(i, fn)) { ++n; Log("%s: XInputGetState (%08X) hooked", dlls[i], fn); }
        else Log("%s: failed to hook %08X", dlls[i], fn);
    }
    return n;
}

// ---------------- keyboard (no controller) ----------------
// On keyboard the game requests A/D pressed multiple times (W/S on up/down prompts). While the extra
// key is pressed and there is an active analog prompt, the mod alternates A and D with SendInput (scan
// code, which DirectInput sees). When releasing the key or ending the prompt, it releases the simulated key.
// releases 'oldVk' (if != 0) and presses 'newVk' (if != 0) in a single SendInput, so the game never sees both together
static void SwapKey(int oldVk, int newVk) {
    INPUT in[2]; memset(in, 0, sizeof(in));
    int n = 0;
    if (oldVk) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wScan = (WORD)MapVirtualKeyA((UINT)oldVk, MAPVK_VK_TO_VSC);
        in[n].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP; ++n;
    }
    if (newVk) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wScan = (WORD)MapVirtualKeyA((UINT)newVk, MAPVK_VK_TO_VSC);
        in[n].ki.dwFlags = KEYEVENTF_SCANCODE; ++n;
    }
    if (n) SendInput((UINT)n, in, sizeof(INPUT));
}

static bool GameIsForeground() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI KeyLoop(LPVOID) {
    // 1 ms clock for the loop to react to each game frame (loaded dynamically: winmm.lib not required)
    typedef UINT (WINAPI *TBP)(UINT);
    HMODULE mm = LoadLibraryA("winmm.dll");
    if (mm) { TBP f = (TBP)GetProcAddress(mm, "timeBeginPeriod"); if (f) f(1); }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    int down = 0;
    bool wasActive = false;
    for (;;) {
        Sleep(1);
        int mode = 0; u32 obj = 0;
        if ((GetAsyncKeyState(gVk) & 0x8000) && GameIsForeground()) mode = PromptMode(&obj);
        if (!mode) {
            if (down) { SwapKey(down, 0); down = 0; }
            if (wasActive) { wasActive = false; Log("keyboard: stopped simulating"); }
            continue;
        }
        if (!wasActive) { wasActive = true; Log("keyboard: simulating keys (mode=%d)", mode); }
        (void)obj;
        int side = (int)((GetTickCount() / (DWORD)gKeyMs) & 1);        // same cadence as controller (KeyPeriodMs = PadPeriodMs)
        int want = (mode == 2) ? (side ? gKeyUp : gKeyDown) : (side ? gKeyRight : gKeyLeft);    // rotation and left/right use A/D
        if (want != down) { SwapKey(down, want); down = want; }
    }
    return 0;
}

static DWORD WINAPI Init(LPVOID) {
    char ini[MAX_PATH]; HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&Init, &self);
    GetModuleFileNameA(self, ini, MAX_PATH);
    char* s = strrchr(ini, '\\'); if (s) *(s + 1) = 0;
    strcpy(gDir, ini); strcat(ini, "RE5_GrabMash.ini");
    gEnable      = GetPrivateProfileIntA("Main", "Enable", 1, ini);
    gKeyMs       = GetPrivateProfileIntA("Main", "KeyPeriodMs", 60, ini);
    gPadMs       = GetPrivateProfileIntA("Main", "PadPeriodMs", 60, ini);
    gPadRotMs    = GetPrivateProfileIntA("Main", "PadRotatePeriodMs", 700, ini);
    gGraceMs     = GetPrivateProfileIntA("Main", "GraceMs", 700, ini);
    gAmp         = GetPrivateProfileIntA("Main", "Amplitude", 30000, ini);
    gPlayer      = GetPrivateProfileIntA("Main", "PlayerId", -1, ini);
    gMaxPromptMs = GetPrivateProfileIntA("Main", "MaxPromptMs", 10000, ini);
    gLog         = GetPrivateProfileIntA("Main", "Log", 0, ini);
    gVk          = GetPrivateProfileIntA("Main", "ExtraKeyVK", 0, ini);
    gKeyLeft     = GetPrivateProfileIntA("Main", "KeyLeftVK", 'A', ini);
    gKeyRight    = GetPrivateProfileIntA("Main", "KeyRightVK", 'D', ini);
    gKeyUp       = GetPrivateProfileIntA("Main", "KeyUpVK", 'W', ini);
    gKeyDown     = GetPrivateProfileIntA("Main", "KeyDownVK", 'S', ini);
    if (gKeyMs < 10) gKeyMs = 10;
    if (gPadMs < 10) gPadMs = 10;
    if (gPadRotMs < 100) gPadRotMs = 100;
    if (gGraceMs < 0) gGraceMs = 0;
    if (gAmp > 32767) gAmp = 32767;
    if (gAmp < 1) gAmp = 1;
    Log("RE5_GrabMash v8 loaded (KeyPeriodMs=%d PadPeriodMs=%d PadRotatePeriodMs=%d GraceMs=%d Amplitude=%d PlayerId=%d MaxPromptMs=%d ExtraKeyVK=%d)",
        gKeyMs, gPadMs, gPadRotMs, gGraceMs, gAmp, gPlayer, gMaxPromptMs, gVk);
    if (!gEnable) return 0;

    if (gVk) {
        if (gVk == gKeyLeft || gVk == gKeyRight || gVk == gKeyUp || gVk == gKeyDown)
            Log("WARNING: ExtraKeyVK (%d) is equal to one of the simulated keys; keyboard-without-controller will remain disabled. Use another key (e.g., F = 70).", gVk);
        else
            CreateThread(NULL, 0, KeyLoop, NULL, 0, NULL);
    }

    bool reqOk = false;
    for (int i = 0; i < 480; ++i) {                             // up to ~4 min (packer takes time; XInput might load later)
        if (!reqOk && InstallReqHook()) { reqOk = true; Log("prompt request hook installed at %08X", FN_REQCTOR); }
        HookXInput();
        Sleep(500);
    }
    if (!reqOk) Log("FAILED: prompt request signature does not match at %08X", FN_REQCTOR);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(NULL, 0, Init, NULL, 0, NULL); }
    return TRUE;
}