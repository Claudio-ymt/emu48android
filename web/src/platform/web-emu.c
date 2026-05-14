// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// Created by: Claudio_ymt
// =====================================================================
//  web-emu.c
//  --------------------------------------------------------------------
//  Web port of Emu48: replaces android-emu.c (the "Emu48.c rewrite"
//  from the dgis Android port) and the JNI bridge from emu-jni.c.
//
//  Responsibilities:
//    * Provide the same set of global variables / critical sections
//      that the rest of the core expects.
//    * Provide draw/buttonDown/buttonUp/keyDown/keyUp public API,
//      and OnPaint/OnLButton/OnKey internal handlers (taken from
//      android-emu.c, but with the JNI bitmap path replaced).
//    * Provide a main() that initializes everything, opens the KML
//      skin + ROM, starts the emulator worker thread, and drives the
//      SDL2/Emscripten event-and-render loop.
//    * Provide stubs for Android-only platform calls (serial port,
//      content-resolver file I/O, haptics, JNI helpers) that the
//      win32-layer.c references.
//    * Provide the global RGBA pixel buffer (webMainScreenPixels) that
//      win32-layer.c writes into during BitBlt / StretchBlt.
//
//  THIS IS A FIRST-PASS MVP. Rendering is wired up but display refresh,
//  precise key mapping, save/load and audio are intentionally minimal.
//  The goal at this stage is "compile, link, show the skin, dispatch
//  clicks." Polish comes in later iterations.
// =====================================================================

#include "core/pch.h"
#include "core/resource.h"
#include "emu.h"
#include "core/io.h"
#include "core/kml.h"
#include "core/debugger.h"
#include "win32-layer.h"
#include <errno.h>  

// SDL detects MSVC via _MSC_VER and tries to #include <sal.h>, which
// doesn't exist in the Emscripten toolchain. The fake _MSC_VER we set
// in win32-layer.h is needed for the Emu48 core but must be hidden
// from SDL. Undefine around the SDL include, then restore.
#ifdef _MSC_VER
#  define _SAVED_MSC_VER _MSC_VER
#  undef _MSC_VER
#endif
#include <SDL2/SDL.h>
#ifdef _SAVED_MSC_VER
#  define _MSC_VER _SAVED_MSC_VER
#  undef _SAVED_MSC_VER
#endif

#include <emscripten.h>
#include <emscripten/html5.h>


// Flag set by OnPaint to indicate the current BitBlt/StretchBlt should
// route to the visible window buffer (webMainScreenPixels). When 0,
// BitBlt/StretchBlt treats all destinations as offscreen — even those
// with hdcCompatible==NULL (e.g. hWindowDC) — so internal display
// updates from the CPU loop don't clobber the visible buffer.
int g_inOnPaint = 0;


// ---------------------------------------------------------------------
// Global emulator state (mirrors android-emu.c)
// ---------------------------------------------------------------------
LPTSTR szAppName = _T("Emu48");
LPTSTR szTopic   = _T("Stack");
LPTSTR szTitle   = NULL;

CRITICAL_SECTION csGDILock;
CRITICAL_SECTION csLcdLock;
CRITICAL_SECTION csKeyLock;
CRITICAL_SECTION csIOLock;
CRITICAL_SECTION csT1Lock;
CRITICAL_SECTION csT2Lock;
CRITICAL_SECTION csTxdLock;
CRITICAL_SECTION csRecvLock;
CRITICAL_SECTION csSlowLock;
CRITICAL_SECTION csDbgLock;

INT              nArgc;
LPCTSTR          *ppArgv;
LARGE_INTEGER    lFreq;
LARGE_INTEGER    lAppStart;
DWORD            idDdeInst;
UINT             uCF_HpObj;
HANDLE           hThread;
HANDLE           hEventShutdn;

HINSTANCE        hApp = NULL;
HWND             hWnd = NULL;
HWND             hDlgDebug = NULL;
HWND             hDlgFind = NULL;
HWND             hDlgProfile = NULL;
HWND             hDlgRplObjView = NULL;
HDC              hWindowDC = NULL;
HPALETTE         hPalette = NULL;
HPALETTE         hOldPalette = NULL;
DWORD            dwTColor = (DWORD) -1;
DWORD            dwTColorTol = 0;
HRGN             hRgn = NULL;
HCURSOR          hCursorArrow = NULL;
HCURSOR          hCursorHand = NULL;
UINT             uWaveDevId = WAVE_MAPPER;
DWORD            dwWakeupDelay = 200;
BOOL             bAutoSave = FALSE;
BOOL             bAutoSaveOnExit = TRUE;
BOOL             bSaveDefConfirm = TRUE;
BOOL             bStartupBackup = FALSE;
BOOL             bAlwaysDisplayLog = TRUE;
BOOL             bLoadObjectWarning = TRUE;
BOOL             bShowTitle = TRUE;
BOOL             bShowMenu = TRUE;
BOOL             bAlwaysOnTop = FALSE;
BOOL             bActFollowsMouse = FALSE;
BOOL             bClientWinMove = FALSE;
BOOL             bSingleInstance = FALSE;

// ---------------------------------------------------------------------
// Symbols normally provided by emu-jni.c (kept here so the core links)
// ---------------------------------------------------------------------
enum DialogBoxMode currentDialogBoxMode;
LPBYTE pbyRomBackup = NULL;
enum ChooseKmlMode chooseCurrentKmlMode;
TCHAR szChosenCurrentKml[MAX_PATH];
TCHAR szKmlLog[10240];
TCHAR szKmlLogBackup[10240];
TCHAR szKmlTitle[10240];
BOOL securityExceptionOccured = FALSE;
BOOL kmlFileNotFound = FALSE;
BOOL settingsPort2en = FALSE;
BOOL settingsPort2wr = FALSE;
BOOL soundAvailable = FALSE;
BOOL soundEnabled = FALSE;
BOOL serialPortSlowDown = FALSE;

// ---------------------------------------------------------------------
// Global pixel buffer for the main screen — written by BitBlt/StretchBlt
// in win32-layer.c, uploaded to an SDL_Texture by the render loop here.
// ---------------------------------------------------------------------
void *webMainScreenPixels = NULL;
int   webMainScreenWidth  = 0;
int   webMainScreenHeight = 0;
int   webMainScreenStride = 0;
static SDL_Window   *gWindow   = NULL;
static SDL_Renderer *gRenderer = NULL;
static SDL_Texture  *gTexture  = NULL;
static int gNeedRedraw = 1;

// ---------------------------------------------------------------------
// Platform stubs for symbols that android-layer.c / emu-jni.c provided
// ---------------------------------------------------------------------

int  openSerialPort(LPCTSTR name) { (void)name; return -1; }
int  closeSerialPort(int id) { (void)id; return 0; }
int  readSerialPort(int id, LPBYTE buf, int len) { (void)id; (void)buf; (void)len; return 0; }
int  writeSerialPort(int id, LPBYTE buf, int len) { (void)id; (void)buf; (void)len; return len; }
int  openFileFromContentResolver(const TCHAR *url, int access) { (void)url; (void)access; return -1; }
int  closeFileFromContentResolver(int fd) { (void)fd; return 0; }
int  openFileInFolderFromContentResolver(const TCHAR *name, const TCHAR *folder, int access) {
    (void)name; (void)folder; (void)access; return -1;
}
void performHapticFeedback(void) { /* no haptics on web */ }
int  mainViewCallback(int type, int p1, int p2) { (void)type; (void)p1; (void)p2; return 0; }
void mainViewResizeCallback(int w, int h) {
    if (w <= 0 || h <= 0) return;
    int stride = w * 4;
    void *newBuf = realloc(webMainScreenPixels, (size_t)stride * h);
    if (!newBuf) return;
    webMainScreenPixels = newBuf;
    webMainScreenWidth  = w;
    webMainScreenHeight = h;
    webMainScreenStride = stride;
    memset(webMainScreenPixels, 0, (size_t)stride * h);

    // Resize the SDL window/canvas to match the skin's real size
    if (gWindow) {
        SDL_SetWindowSize(gWindow, w, h);
    }
    if (gTexture) {
        SDL_DestroyTexture(gTexture);
        gTexture = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_ABGR8888,
                                     SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    LOGD("Canvas resized to %dx%d", w, h);
    gNeedRedraw = 1;
}
// ---------------------------------------------------------------------
// Additional stubs for symbols normally provided by android-layer.c /
// emu-jni.c / debugger.c / udp.c (all excluded from the web build).
// ---------------------------------------------------------------------

// android-layer.c symbols
void mainViewUpdateCallback(void) { gNeedRedraw = 1; }
int  setSerialPortParameters(int id, int baudRate) { (void)id; (void)baudRate; return 0; }
int  serialPortPurgeComm(int id, int flags) { (void)id; (void)flags; return 0; }
int  serialPortSetBreak(int id) { (void)id; return 0; }
int  serialPortClearBreak(int id) { (void)id; return 0; }

// emu-jni.c symbols
int  showAlert(const TCHAR *messageText, int flags) {
    (void)flags;
    LOGD("Alert: %s", messageText ? messageText : "(null)");
    return 0;
}
void sendMenuItemCommand(int menuItem) { (void)menuItem; }
BOOL getFirstKMLFilenameForType(BYTE chipsetType) { (void)chipsetType; return FALSE; }
void setKMLIcon(int w, int h, LPBYTE buffer, int bufferSize) {
    (void)w; (void)h; (void)buffer; (void)bufferSize;
}

// debugger.c symbols (debugger excluded from web build)
VOID DisableDebugger(VOID) { }
BOOL CheckBreakpoint(DWORD dwAddr, DWORD dwRange, UINT nType) {
    (void)dwAddr; (void)dwRange; (void)nType; return FALSE;
}
VOID NotifyDebugger(INT nType) { (void)nType; }
VOID UpdateDbgCycleCounter(VOID) { }

// udp.c symbol (UDP excluded from web build)
BOOL SendByteUdp(BYTE byte) { (void)byte; return FALSE; }





















// ---------------------------------------------------------------------
// Stubs for window-status / clipboard (same as android-emu.c)
// ---------------------------------------------------------------------
VOID SetWindowTitle(LPCTSTR szString) { (void)szString; }
VOID ForceForegroundWindow(HWND h)    { (void)h; }
VOID CopyItemsToClipboard(HWND h)     { (void)h; }

// ---------------------------------------------------------------------
// Transparency helper (copied from android-emu.c, JNI bitmap removed —
// operates directly on the global RGBA buffer)
// ---------------------------------------------------------------------
#define WIDTHBYTES(bits) ((((bits) + 31) / 32) * 4)

static BOOL AbsColorCmp(DWORD c1, DWORD c2, DWORD tol) {
    DWORD d;
    d  = (DWORD) abs((INT)(c1 & 0xFF) - (INT)(c2 & 0xFF)); c1 >>= 8; c2 >>= 8;
    d += (DWORD) abs((INT)(c1 & 0xFF) - (INT)(c2 & 0xFF)); c1 >>= 8; c2 >>= 8;
    d += (DWORD) abs((INT)(c1 & 0xFF) - (INT)(c2 & 0xFF));
    return d > tol;
}

static BOOL LabColorCmp(DWORD c1, DWORD c2, DWORD tol) {
    INT dc; DWORD d;
    dc = (INT)(c1 & 0xFF) - (INT)(c2 & 0xFF); d  = (DWORD)(dc*dc); c1 >>= 8; c2 >>= 8;
    dc = (INT)(c1 & 0xFF) - (INT)(c2 & 0xFF); d += (DWORD)(dc*dc); c1 >>= 8; c2 >>= 8;
    dc = (INT)(c1 & 0xFF) - (INT)(c2 & 0xFF); d += (DWORD)(dc*dc);
    tol *= tol;
    return d > tol;
}

static DWORD EncodeColorBits(DWORD val, DWORD mask) {
#define MAXBIT 32
    UINT uL = MAXBIT, uR = 8;
    DWORD bm = mask;
    val &= 0xFF;
    while ((bm & (1U << (MAXBIT - 1))) == 0 && uL > 0) { bm <<= 1; --uL; }
    if (uL > 24) { uL -= uR; uR = 0; }
    return ((val << uL) >> uR) & mask;
#undef MAXBIT
}

static void MakeBitmapTransparent(HBITMAP hBmp, COLORREF color, DWORD dwTol) {
    (void)hBmp;
    BOOL (*cmp)(DWORD, DWORD, DWORD);
    if (dwTol >= 1000) { cmp = LabColorCmp; dwTol -= 1000; }
    else               { cmp = AbsColorCmp; }

    if (!webMainScreenPixels) return;

    LPBYTE bits = (LPBYTE) webMainScreenPixels;
    DWORD red = 0x00FF0000, green = 0x0000FF00, blue = 0x000000FF;
    color = EncodeColorBits((color >>  0), blue)
          | EncodeColorBits((color >>  8), green)
          | EncodeColorBits((color >> 16), red);

    LPBYTE p = bits + (webMainScreenHeight - 1) * webMainScreenStride;
    for (LONG y = 0; y < webMainScreenHeight; ++y) {
        LPBYTE start = p;
        for (LONG x = 0; x < webMainScreenWidth; ++x) {
            if (!cmp(*(LPDWORD)p, color, dwTol))
                *(LPDWORD)p = 0x00000000;
            p += 4;
        }
        p = start - webMainScreenStride;
    }
}

// ---------------------------------------------------------------------
// Paint, click and key handlers — straight from android-emu.c
// ---------------------------------------------------------------------
static LRESULT OnPaint(HWND hWindow) {
    PAINTSTRUCT Paint;
    HDC hPaintDC;
    hPaintDC = BeginPaint(hWindow, &Paint);
    g_inOnPaint = 1; 

    static int s_onpaint_ctr = 0;
    s_onpaint_ctr++;
    if (s_onpaint_ctr <= 3) {
        LOGD("OnPaint #%d: hPaintDC=%p Paint.rcPaint=(%d,%d,%d,%d) hMainDC=%p hLcdDC=%p",
             s_onpaint_ctr, (void*)hPaintDC,
             Paint.rcPaint.left, Paint.rcPaint.top, Paint.rcPaint.right, Paint.rcPaint.bottom,
             (void*)hMainDC, (void*)hLcdDC);
    }

   if (hMainDC != NULL)
    {
        RECT rcMainPaint = Paint.rcPaint;   
        rcMainPaint.left   += nBackgroundX;
        rcMainPaint.top    += nBackgroundY;
        rcMainPaint.right  += nBackgroundX;
        rcMainPaint.bottom += nBackgroundY;
     

        EnterCriticalSection(&csGDILock);
        {
            UINT nLines = MAINSCREENHEIGHT;
            BitBlt(hPaintDC, Paint.rcPaint.left, Paint.rcPaint.top,
                   Paint.rcPaint.right - Paint.rcPaint.left,
                   Paint.rcPaint.bottom - Paint.rcPaint.top,
                   hMainDC, rcMainPaint.left, rcMainPaint.top, SRCCOPY);

            if (dwTColor != (DWORD)-1)
                MakeBitmapTransparent((HBITMAP)GetCurrentObject(hPaintDC, OBJ_BITMAP),
                                      dwTColor, dwTColorTol);

            SetWindowOrgEx(hPaintDC, nBackgroundX, nBackgroundY, NULL);

            StretchBlt(hPaintDC, nLcdX, nLcdY,
                       131 * nLcdZoom * nGdiXZoom, Chipset.d0size * nLcdZoom * nGdiYZoom,
                       hLcdDC, Chipset.d0offset, 0, 131, Chipset.d0size, SRCCOPY);
            StretchBlt(hPaintDC, nLcdX, nLcdY + Chipset.d0size * nLcdZoom * nGdiYZoom,
                       131 * nLcdZoom * nGdiXZoom, nLines * nLcdZoom * nGdiYZoom,
                       hLcdDC, Chipset.boffset, Chipset.d0size, 131, nLines, SRCCOPY);
            StretchBlt(hPaintDC, nLcdX, nLcdY + (nLines + Chipset.d0size) * nLcdZoom * nGdiYZoom,
                       131 * nLcdZoom * nGdiXZoom, MENUHEIGHT * nLcdZoom * nGdiYZoom,
                       hLcdDC, 0, (nLines + Chipset.d0size), 131, MENUHEIGHT, SRCCOPY);
            GdiFlush();
        }
        LeaveCriticalSection(&csGDILock);
        UpdateAnnunciators(0x3F);
        RefreshButtons(&rcMainPaint);
    }
    EndPaint(hWindow, &Paint);
    gNeedRedraw = 1;
    g_inOnPaint = 0; 
    return 0;
}

static LRESULT OnLButtonDown(UINT nFlags, WORD x, WORD y) {
    if (nMacroState == MACRO_PLAY) return 0;
    if (nState == SM_RUN) {
        MouseButtonDownAt(nFlags, x, y);
        if (MouseIsButton(x, y)) {
            performHapticFeedback();
            return 1;
        }
    }
    return 0;
}

static LRESULT OnLButtonUp(UINT nFlags, WORD x, WORD y) {
    if (nMacroState == MACRO_PLAY) return 0;
    if (nState == SM_RUN) MouseButtonUpAt(nFlags, x, y);
    return 0;
}

static LRESULT OnKeyDown(int nVirtKey, LPARAM lKeyData) {
    if (nMacroState == MACRO_PLAY) return 0;
    if (nState == SM_RUN && (lKeyData & 0x40000000) == 0)
        RunKey((BYTE)nVirtKey, TRUE);
    return 0;
}

static LRESULT OnKeyUp(int nVirtKey, LPARAM lKeyData) {
    if (nMacroState == MACRO_PLAY) return 0;
    if (nState == SM_RUN) RunKey((BYTE)nVirtKey, FALSE);
    (void)lKeyData;
    return 0;
}

// Public API (used by win32-layer.c via extern declarations)
void draw(void) {
    static int s_draw_log_ctr = 0;
    s_draw_log_ctr++;
    if (s_draw_log_ctr <= 3 || (s_draw_log_ctr % 60) == 0) {
        LOGD("draw() call #%d: hMainDC=%p hLcdDC=%p hWindowDC=%p nBackgroundW=%d nBackgroundH=%d nLcdX=%d nLcdY=%d",
             s_draw_log_ctr,
             (void*)hMainDC, (void*)hLcdDC, (void*)hWindowDC,
             nBackgroundW, nBackgroundH, nLcdX, nLcdY);
    }
    OnPaint(NULL);
}

BOOL buttonDown(int x, int y)   { return OnLButtonDown(MK_LBUTTON, x, y); }
void buttonUp(int x, int y)     { OnLButtonUp(MK_LBUTTON, x, y); }
void keyDown(int virtKey)       { OnKeyDown(virtKey, 0); }
void keyUp(int virtKey)         { OnKeyUp(virtKey, 0); }

// ---------------------------------------------------------------------
// Main loop driven by Emscripten
// ---------------------------------------------------------------------

// =====================================================================
// web_yield_to_browser
// --------------------------------------------------------------------
// Called from inside the CPU opcode loop (every ~8192 opcodes) via
// engine.c's Emscripten hook. Each invocation:
//   1. Blits the emulator's internal DCs into our pixel buffer (draw)
//   2. Pushes that buffer to the SDL canvas
//   3. Drains SDL event queue (mouse, keyboard)
//   4. Yields to the browser event loop via emscripten_sleep(0)
// This is the equivalent of "one frame" of the emulator under WASM.
// =====================================================================
void web_yield_to_browser(void) {
    // 0. Pump pending timers (display update timer, battery, etc.)
    //    Replacement for the background thread that doesn't work under
    //    single-threaded Asyncify.
    extern void web_pump_timers(void);
    web_pump_timers();

   // 1. Paint emulator state into our pixel buffer
    if (nState == SM_RUN) {
        draw();
    }

    // 2. Push buffer to the SDL canvas
    if (webMainScreenPixels && gTexture && gRenderer) {
        SDL_UpdateTexture(gTexture, NULL, webMainScreenPixels, webMainScreenStride);
        SDL_RenderClear(gRenderer);
        SDL_RenderCopy(gRenderer, gTexture, NULL, NULL);
        SDL_RenderPresent(gRenderer);
    }

    // 3. Drain SDL events
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    buttonDown(ev.button.x, ev.button.y);
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    buttonUp(ev.button.x, ev.button.y);
                break;
            case SDL_KEYDOWN: keyDown(ev.key.keysym.sym & 0xFF); break;
            case SDL_KEYUP:   keyUp(ev.key.keysym.sym & 0xFF);   break;
            default: break;
        }
    }

    // 4. Yield to browser
    emscripten_sleep(0);
}


static void mainLoopIter(void) {
    
	// Unused after CPU starts — WorkerThread takes over the event loop.
	 // Kept as a placeholder in case we want a pre-CPU phase later.
    if (webMainScreenPixels && gTexture && gRenderer) {
        SDL_UpdateTexture(gTexture, NULL, webMainScreenPixels, webMainScreenStride);
        SDL_RenderClear(gRenderer);
        SDL_RenderCopy(gRenderer, gTexture, NULL, NULL);
        SDL_RenderPresent(gRenderer);
    }
}

// ---------------------------------------------------------------------
// Equivalent of Java_..._NativeLib_start from emu-jni.c
// ---------------------------------------------------------------------
extern void win32Init(void);
extern UINT WorkerThread(LPVOID pParam);
extern UINT nState;
extern UINT nNextState;
extern UINT nState;
extern UINT nNextState;

static int emulatorStart(const char *kmlPath) {
    chooseCurrentKmlMode = ChooseKmlMode_UNKNOWN;
    szChosenCurrentKml[0] = '\0';

    InitializeCriticalSection(&csGDILock);
    InitializeCriticalSection(&csLcdLock);
    InitializeCriticalSection(&csKeyLock);
    InitializeCriticalSection(&csIOLock);
    InitializeCriticalSection(&csT1Lock);
    InitializeCriticalSection(&csT2Lock);
    InitializeCriticalSection(&csTxdLock);
    InitializeCriticalSection(&csRecvLock);
    InitializeCriticalSection(&csSlowLock);
    InitializeCriticalSection(&csDbgLock);

    GetCurrentDirectory(ARRAYSIZEOF(szCurrentDirectory), szCurrentDirectory);
    _tcscpy(szCurrentDirectory, "");
    _tcscpy(szEmuDirectory, "assets/");
    _tcscpy(szRomDirectory, "assets/");
    _tcscpy(szPort2Filename, "");

    QueryPerformanceFrequency(&lFreq);
    QueryPerformanceCounter(&lAppStart);

    hWnd = CreateWindow();
    hWindowDC = GetDC(hWnd);

    szCurrentKml[0] = 0;
    SetSpeed(bRealSpeed);

    hEventShutdn = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!hEventShutdn) {
        LOGE("Event creation failed");
        return -1;
    }

    // Start with CPU stopped — InitKML insists on SM_INVALID before it
    // will (re)initialize. We'll flip to SM_RUN after the skin/ROM map.
    nState     = SM_INVALID;
    nNextState = SM_INVALID;
    hThread    = NULL;

    soundEnabled = soundAvailable = FALSE;

    LOGD("=== loading KML: %s ===", kmlPath);

    // Sanity check: confirm the file actually exists in the virtual FS
    FILE *test = fopen(kmlPath, "rb");
    if (test) {
        fseek(test, 0, SEEK_END);
        long sz = ftell(test);
        fclose(test);
        LOGD("KML file found, size=%ld bytes", sz);
    } else {
        LOGE("KML file NOT FOUND at %s (errno=%d)", kmlPath, errno);
    }

// Also probe the include file the KML will reference
    const char *kmiPaths[] = {
        "/assets/keyb4950.kmi",
        "keyb4950.kmi",
        "assets/keyb4950.kmi",
        NULL
    };
    for (int i = 0; kmiPaths[i]; ++i) {
        FILE *t2 = fopen(kmiPaths[i], "rb");
        if (t2) {
            fseek(t2, 0, SEEK_END);
            long sz = ftell(t2);
            fclose(t2);
            LOGD("KMI probe '%s' -> FOUND, size=%ld", kmiPaths[i], sz);
        } else {
            LOGE("KMI probe '%s' -> NOT FOUND (errno=%d)", kmiPaths[i], errno);
        }
    }

    _tcscpy(szCurrentKml, kmlPath);
    BOOL bSucc = InitKML(szCurrentKml, FALSE);
    if (!bSucc) {
        LOGE("InitKML failed for %s", kmlPath);
        LOGE("--- KML parser log ---");
        LOGE("%s", szKmlLog);
        LOGE("--- end KML log ---");
        return -1;
    }
    LOGD("KML loaded OK. Title=%s", szKmlTitle);

    // -----------------------------------------------------------------
    // Inline equivalent of the data-init half of NewDocument(). We
    // cannot call NewDocument() directly because it depends on the
    // GUI chooser (DisplayChooseKml). InitKML above has already set
    // cCurrentRomType; everything below mirrors files.c lines 884..964
    // but skips the SaveBackup / breakpoint-load steps.
    // -----------------------------------------------------------------
    {
        extern LPBYTE Port0, Port1, Port2;
        extern BOOL   bPort2Writeable;
        extern VOID   FlashInit(VOID);                  // i28f160.c

        Chipset.type = cCurrentRomType;
        CrcRom(&Chipset.wRomCrc);                       // CrcRom from Emu48.h

        if (Chipset.type == '6' || Chipset.type == 'A') {           // HP38G
            Chipset.Port0Size    = (Chipset.type == 'A') ? 32 : 64;
            Chipset.Port1Size    = 0;
            Chipset.Port2Size    = 0;
            Chipset.cards_status = 0x0;
        }
        if (Chipset.type == 'E' || Chipset.type == 'P') {           // HP39/40G/HP39G+
            Chipset.Port0Size    = 128;
            Chipset.Port1Size    = 0;
            Chipset.Port2Size    = 128;
            Chipset.cards_status = 0xF;
            bPort2Writeable      = TRUE;
        }
        if (Chipset.type == 'S') {                                   // HP48SX
            Chipset.Port0Size    = 32;
            Chipset.Port1Size    = 128;
            Chipset.Port2Size    = 0;
            Chipset.cards_status = 0x5;
        }
        if (Chipset.type == 'G') {                                   // HP48GX
            Chipset.Port0Size    = 128;
            Chipset.Port1Size    = 128;
            Chipset.Port2Size    = 0;
            Chipset.cards_status = 0xA;
        }
        if (Chipset.type == 'X' || Chipset.type == '2' || Chipset.type == 'Q') {
            // HP49G / HP48gII / HP49g+ / HP50g
            Chipset.Port0Size    = 256;
            Chipset.Port1Size    = 128;
            Chipset.Port2Size    = 128;
            Chipset.cards_status = 0xF;
            bPort2Writeable      = TRUE;
            FlashInit();                                // flash structure for 49g+/50g
        }
        if (Chipset.type == 'Q') {                                   // HP49g+/50g
            Chipset.d0size = 16;
        }
        Chipset.IORam[LPE] = RST;                       // ReSeT bit at power on

        // Allocate Port0/1/2 backing storage. calloc zeroes the buffer,
        // matching the calculator's power-on RAM state.
        if (Chipset.Port0Size) {
            Port0 = (LPBYTE) calloc(Chipset.Port0Size * 2048, sizeof(*Port0));
            if (!Port0) { LOGE("Port0 calloc(%u) failed", (unsigned)(Chipset.Port0Size*2048)); return -1; }
        }
        if (Chipset.Port1Size) {
            Port1 = (LPBYTE) calloc(Chipset.Port1Size * 2048, sizeof(*Port1));
            if (!Port1) { LOGE("Port1 calloc(%u) failed", (unsigned)(Chipset.Port1Size*2048)); return -1; }
        }
        if (Chipset.Port2Size) {
            Port2 = (LPBYTE) calloc(Chipset.Port2Size * 2048, sizeof(*Port2));
            if (!Port2) { LOGE("Port2 calloc(%u) failed", (unsigned)(Chipset.Port2Size*2048)); return -1; }
        }
        // LoadBreakpointList(NULL) skipped: debugger module not linked in web build.

        LOGD("=== inline NewDocument done: type='%c'(0x%02X) "
             "P0=%uKB@%p  P1=%uKB@%p  P2=%uKB@%p  d0size=%u  cards=0x%X ===",
             Chipset.type, (unsigned)Chipset.type,
             (unsigned)(Chipset.Port0Size*2), Port0,
             (unsigned)(Chipset.Port1Size*2), Port1,
             (unsigned)(Chipset.Port2Size*2), Port2,
             (unsigned)Chipset.d0size,
             (unsigned)Chipset.cards_status);
    }

    // Now we can flip the state machine to running.
    nState     = SM_RUN;
    nNextState = SM_RUN;

    mainViewResizeCallback(nBackgroundW, nBackgroundH);
    draw();
    LOGD("=== first draw() called ===");
    return 0;
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    LOGD("=== win32Init() done ==="); 
    win32Init();
    LOGD("=== win32Init() done ==="); 
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Initial window — will be resized once the skin is loaded.
    gWindow = SDL_CreateWindow("Emu48 Web",
                               SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                               400, 700, SDL_WINDOW_SHOWN);
    gRenderer = SDL_CreateRenderer(gWindow, -1, SDL_RENDERER_ACCELERATED);
   gTexture  = SDL_CreateTexture(gRenderer, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_STREAMING, 400, 700);

if (emulatorStart("/assets/real50g-l.kml") != 0) {
        LOGE("Emulator start failed");
        return 1;
    }

    // Initial render so the skin appears even before the CPU starts
    web_yield_to_browser();

    // Force WorkerThread to run the init block (RomSwitch / StartTimers /
    // StartDisplay / UpdateMainDisplay). On native Emu48 nState starts as
    // SM_INVALID via the thread creation path; in the web build we drive
    // WorkerThread inline, so we set it explicitly here.
    nState     = 0xFFFFFFFF;   // anything != SM_RUN
    nNextState = 0;            // SM_RUN (= 0 on this codebase)
    // === DIAGNOSTIC: confirm Port0/1/2 allocation status ===
    {
        extern LPBYTE Port0, Port1, Port2;
        LOGD("=== PORT STATUS BEFORE CPU START ===");
        LOGD("    Chipset.type='%c' (0x%02X)", Chipset.type, (unsigned)Chipset.type);
        LOGD("    Port0Size=%u  Port0=%p  %s",
             (unsigned)Chipset.Port0Size, Port0,
             Port0 ? "ALLOCATED" : "*** NULL - RAM NOT MAPPED ***");
        LOGD("    Port1Size=%u  Port1=%p  %s",
             (unsigned)Chipset.Port1Size, Port1,
             Port1 ? "ALLOCATED" : "(null)");
        LOGD("    Port2Size=%u  Port2=%p  %s",
             (unsigned)Chipset.Port2Size, Port2,
             Port2 ? "ALLOCATED" : "(null)");
        LOGD("    pbyPort2=%p  bPort2Writeable=%d  dwPort2Size=%u  dwPort2Mask=0x%08X",
             pbyPort2, (int)bPort2Writeable, (unsigned)dwPort2Size, (unsigned)dwPort2Mask);
    }

    LOGD("=== Entering WorkerThread (CPU drives the event loop now) ===");
    LOGD("    forced nState=0x%X nNextState=%u (so init block will run)", nState, nNextState);
    // WorkerThread is an infinite opcode loop. It calls web_yield_to_browser
    // periodically (from engine.c via Asyncify), which drains events, renders,
    // and yields back to the browser. We never return from this call.
    WorkerThread(NULL);

    // Unreachable
    return 0;
}








