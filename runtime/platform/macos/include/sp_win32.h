/*
 * sp_win32.h - the Win32 constants and 32-bit struct layouts the macOS platform layer speaks.
 *
 * Written from the public Win32/DirectSound documentation (Microsoft Learn); no Windows SDK
 * headers are used. Every struct here has the x86 (guest) layout, so a runtime adapter can copy
 * it to and from guest memory unchanged. Pointers inside guest structs are uint32_t guest
 * addresses, never host pointers.
 */
#ifndef SP_WIN32_H
#define SP_WIN32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t sp_hwnd;   /* guest HWND value handed out by the platform layer */
typedef int32_t  sp_hresult;

/* ---- window messages ---------------------------------------------------------------------- */
#define WM_NULL            0x0000
#define WM_CREATE          0x0001
#define WM_DESTROY         0x0002
#define WM_MOVE            0x0003
#define WM_SIZE            0x0005
#define WM_ACTIVATE        0x0006
#define WM_SETFOCUS        0x0007
#define WM_KILLFOCUS       0x0008
#define WM_PAINT           0x000F
#define WM_CLOSE           0x0010
#define WM_QUIT            0x0012
#define WM_ERASEBKGND      0x0014
#define WM_SHOWWINDOW      0x0018
#define WM_SETTINGCHANGE   0x001A
#define WM_ACTIVATEAPP     0x001C
#define WM_SETCURSOR       0x0020
#define WM_GETMINMAXINFO   0x0024
#define WM_INPUTLANGCHANGE 0x0051
#define WM_CONTEXTMENU     0x007B
#define WM_DISPLAYCHANGE   0x007E
#define WM_NCHITTEST       0x0084
#define WM_NCACTIVATE      0x0086
#define WM_KEYDOWN         0x0100
#define WM_KEYUP           0x0101
#define WM_CHAR            0x0102
#define WM_DEADCHAR        0x0103
#define WM_SYSKEYDOWN      0x0104
#define WM_SYSKEYUP        0x0105
#define WM_SYSCHAR         0x0106
#define WM_KEYFIRST        0x0100
#define WM_KEYLAST         0x0109
#define WM_TIMER           0x0113
#define WM_MENUCHAR        0x0120
#define WM_MOUSEMOVE       0x0200
#define WM_LBUTTONDOWN     0x0201
#define WM_LBUTTONUP       0x0202
#define WM_LBUTTONDBLCLK   0x0203
#define WM_RBUTTONDOWN     0x0204
#define WM_RBUTTONUP       0x0205
#define WM_RBUTTONDBLCLK   0x0206
#define WM_MBUTTONDOWN     0x0207
#define WM_MBUTTONUP       0x0208
#define WM_MBUTTONDBLCLK   0x0209
#define WM_MOUSEWHEEL      0x020A
#define WM_MOUSEFIRST      0x0200
#define WM_MOUSELAST       0x020D
#define WM_SIZING          0x0214
#define WM_CAPTURECHANGED  0x0215
#define WM_ENTERSIZEMOVE   0x0231
#define WM_EXITSIZEMOVE    0x0232
#define WM_MOUSELEAVE      0x02A3
#define WM_USER            0x0400

/* PeekMessage flags */
#define PM_NOREMOVE 0x0000
#define PM_REMOVE   0x0001
#define PM_NOYIELD  0x0002

/* WM_ACTIVATE / WM_SIZE / hit-test values */
#define WA_INACTIVE    0
#define WA_ACTIVE      1
#define WA_CLICKACTIVE 2
#define SIZE_RESTORED  0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define HTNOWHERE 0
#define HTCLIENT  1

/* mouse key-state flags in wParam */
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT   0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define WHEEL_DELTA 120

/* ---- virtual-key codes ---------------------------------------------------------------------- */
#define VK_LBUTTON  0x01
#define VK_RBUTTON  0x02
#define VK_MBUTTON  0x04
#define VK_BACK     0x08
#define VK_TAB      0x09
#define VK_CLEAR    0x0C
#define VK_RETURN   0x0D
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_PAUSE    0x13
#define VK_CAPITAL  0x14
#define VK_KANA     0x15
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_PRIOR    0x21
#define VK_NEXT     0x22
#define VK_END      0x23
#define VK_HOME     0x24
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_INSERT   0x2D
#define VK_DELETE   0x2E
#define VK_LWIN     0x5B
#define VK_RWIN     0x5C
#define VK_APPS     0x5D
#define VK_NUMPAD0  0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD      0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL  0x6E
#define VK_DIVIDE   0x6F
#define VK_F1       0x70
#define VK_NUMLOCK  0x90
#define VK_OEM_NEC_EQUAL 0x92
#define VK_LSHIFT   0xA0
#define VK_RSHIFT   0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU    0xA4
#define VK_RMENU    0xA5
#define VK_VOLUME_MUTE 0xAD
#define VK_VOLUME_DOWN 0xAE
#define VK_VOLUME_UP   0xAF
#define VK_OEM_1      0xBA
#define VK_OEM_PLUS   0xBB
#define VK_OEM_COMMA  0xBC
#define VK_OEM_MINUS  0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2      0xBF
#define VK_OEM_3      0xC0
#define VK_OEM_4      0xDB
#define VK_OEM_5      0xDC
#define VK_OEM_6      0xDD
#define VK_OEM_7      0xDE
#define VK_OEM_102    0xE2
#define VK_OEM_ATTN   0xF0

/* ---- structs with the guest (x86) layout ---------------------------------------------------- */
typedef struct sp_point { int32_t x, y; } sp_point;
typedef struct sp_rect  { int32_t left, top, right, bottom; } sp_rect;

/* MSG: 28 bytes on x86 (hwnd, message, wParam, lParam, time, pt). */
typedef struct sp_msg {
    uint32_t hwnd;
    uint32_t message;
    uint32_t wParam;
    int32_t  lParam;
    uint32_t time;
    sp_point pt;
} sp_msg;

/* PAINTSTRUCT: 64 bytes on x86. */
typedef struct sp_paintstruct {
    uint32_t hdc;
    int32_t  fErase;
    sp_rect  rcPaint;
    int32_t  fRestore, fIncUpdate;
    uint8_t  rgbReserved[32];
} sp_paintstruct;

/* MINMAXINFO: 40 bytes. */
typedef struct sp_minmaxinfo {
    sp_point ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
} sp_minmaxinfo;

/* ---- DirectSound -------------------------------------------------------------------------- */
#define WAVE_FORMAT_PCM        0x0001
#define WAVE_FORMAT_EXTENSIBLE 0xFFFE

/* WAVEFORMATEX (18 bytes, packed) followed, for EXTENSIBLE, by 22 more bytes. */
#pragma pack(push, 1)
typedef struct sp_waveformatex {
    uint16_t wFormatTag;
    uint16_t nChannels;
    uint32_t nSamplesPerSec;
    uint32_t nAvgBytesPerSec;
    uint16_t nBlockAlign;
    uint16_t wBitsPerSample;
    uint16_t cbSize;
} sp_waveformatex;
typedef struct sp_waveformatextensible {
    sp_waveformatex Format;
    uint16_t wValidBitsPerSample;
    uint32_t dwChannelMask;
    uint8_t  SubFormat[16];
} sp_waveformatextensible;
#pragma pack(pop)

/* DSBUFFERDESC (36 bytes = 0x24, the dwSize the game passes at 0x113d4a6/0x113d5dd). */
typedef struct sp_dsbufferdesc {
    uint32_t dwSize;
    uint32_t dwFlags;
    uint32_t dwBufferBytes;
    uint32_t dwReserved;
    uint32_t lpwfxFormat;      /* guest address of a WAVEFORMATEX */
    uint8_t  guid3DAlgorithm[16];
} sp_dsbufferdesc;

/* DSCAPS (96 bytes = 0x60, the size the game passes to IDirectSound::GetCaps at 0x113d83d). */
typedef struct sp_dscaps {
    uint32_t dwSize, dwFlags, dwMinSecondarySampleRate, dwMaxSecondarySampleRate;
    uint32_t dwPrimaryBuffers, dwMaxHwMixingAllBuffers, dwMaxHwMixingStaticBuffers;
    uint32_t dwMaxHwMixingStreamingBuffers, dwFreeHwMixingAllBuffers, dwFreeHwMixingStaticBuffers;
    uint32_t dwFreeHwMixingStreamingBuffers, dwMaxHw3DAllBuffers, dwMaxHw3DStaticBuffers;
    uint32_t dwMaxHw3DStreamingBuffers, dwFreeHw3DAllBuffers, dwFreeHw3DStaticBuffers;
    uint32_t dwFreeHw3DStreamingBuffers, dwTotalHwMemBytes, dwFreeHwMemBytes;
    uint32_t dwMaxContigFreeHwMemBytes, dwUnlockTransferRateHwBuffers;
    uint32_t dwPlayCpuOverheadSwBuffers, dwReserved1, dwReserved2;
} sp_dscaps;

/* DSBCAPS (20 bytes). */
typedef struct sp_dsbcaps {
    uint32_t dwSize, dwFlags, dwBufferBytes, dwUnlockTransferRate, dwPlayCpuOverhead;
} sp_dsbcaps;

#define DSSCL_NORMAL        1
#define DSSCL_PRIORITY      2
#define DSSCL_EXCLUSIVE     3
#define DSSCL_WRITEPRIMARY  4

#define DSBCAPS_PRIMARYBUFFER       0x00000001
#define DSBCAPS_STATIC              0x00000002
#define DSBCAPS_LOCSOFTWARE         0x00000008
#define DSBCAPS_CTRLFREQUENCY       0x00000020
#define DSBCAPS_CTRLPAN             0x00000040
#define DSBCAPS_CTRLVOLUME          0x00000080
#define DSBCAPS_GLOBALFOCUS         0x00008000
#define DSBCAPS_GETCURRENTPOSITION2 0x00010000

#define DSCAPS_PRIMARYMONO     0x00000001
#define DSCAPS_PRIMARYSTEREO   0x00000002
#define DSCAPS_PRIMARY8BIT     0x00000004
#define DSCAPS_PRIMARY16BIT    0x00000008
#define DSCAPS_CONTINUOUSRATE  0x00000010
#define DSCAPS_EMULDRIVER      0x00000020
#define DSCAPS_SECONDARYMONO   0x00000100
#define DSCAPS_SECONDARYSTEREO 0x00000200
#define DSCAPS_SECONDARY8BIT   0x00000400
#define DSCAPS_SECONDARY16BIT  0x00000800

#define DSBPLAY_LOOPING 0x00000001
#define DSBSTATUS_PLAYING 0x00000001
#define DSBSTATUS_BUFFERLOST 0x00000002
#define DSBSTATUS_LOOPING 0x00000004
#define DSBLOCK_FROMWRITECURSOR 0x00000001
#define DSBLOCK_ENTIREBUFFER    0x00000002

#define DSBVOLUME_MIN (-10000)
#define DSBVOLUME_MAX 0

#define SP_S_OK               ((sp_hresult)0x00000000)
#define SP_E_NOINTERFACE      ((sp_hresult)0x80004002)
#define SP_E_POINTER          ((sp_hresult)0x80004003)
#define SP_E_FAIL             ((sp_hresult)0x80004005)
#define SP_E_OUTOFMEMORY      ((sp_hresult)0x8007000E)
#define SP_DSERR_INVALIDPARAM ((sp_hresult)0x80070057)
#define SP_DSERR_INVALIDCALL  ((sp_hresult)0x88780032)
#define SP_DSERR_BADFORMAT    ((sp_hresult)0x88780064)
#define SP_DSERR_UNSUPPORTED  ((sp_hresult)0x80004001)
#define SP_DSERR_NODRIVER     ((sp_hresult)0x88780078)
#define SP_DSERR_PRIOLEVELNEEDED ((sp_hresult)0x88780046)

#ifdef __cplusplus
}
#endif
#endif /* SP_WIN32_H */
