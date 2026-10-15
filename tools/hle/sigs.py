"""Calling-convention table for every import and dynamically resolved name.

Written by hand from the public Win32 / MSVC CRT prototypes (our own knowledge of the
documented signatures, not from the game). tools/hle/check_sigs.py cross-checks the
stdcall/cdecl split and the argument-slot counts against the push counts and caller
clean-up at the game's call sites.

Line format:  <name> <cc> <slots> <ret> [flags]
  cc     c = cdecl, s = stdcall, t = thiscall, x = x87 (_CI*: operands on the x87 stack),
         d = data import (a variable, not a function)
  slots  4-byte argument slots (a double or 64-bit integer takes 2). For cdecl varargs the
         fixed slots followed by '+' (e.g. 2+).
  ret    v void, i int/pointer in eax, l 64-bit in edx:eax, f double in ST(0), n noreturn,
         x87 effect for cc x: 'u' unary (ST0 = f(ST0)), 'b' binary (ST1 = f(ST1, ST0); pop)
  flags  ord=<n> for imports by ordinal (name is then the inferred export name)

Usage:   a table, read by gen_thunks.py and check_sigs.py (parse() returns it)
When:    edit it when a shim needs a convention or slot count that is missing or wrong, then run gen_thunks.py.
"""

SIGS = {
"ADVAPI32.dll": """
GetUserNameW s 2 i
RegQueryValueExA s 6 i
RegOpenKeyExA s 5 i
RegSetValueExA s 6 i
RegEnumKeyExW s 8 i
RegQueryValueExW s 6 i
RegisterEventSourceA s 2 i
ReportEventA s 9 i
DeregisterEventSource s 1 i
RegOpenKeyExW s 5 i
RegCloseKey s 1 i
GetUserNameA s 2 i
""",
"DINPUT8.dll": """
DirectInput8Create s 5 i
""",
"DSOUND.dll": """
DirectSoundCreate s 3 i ord=1
GetDeviceID s 2 i ord=9
""",
"GDI32.dll": """
CreateSolidBrush s 1 i
DeleteObject s 1 i
ExtEscape s 6 i
DeleteDC s 1 i
GetDeviceCaps s 2 i
CreateDCA s 4 i
""",
"IMM32.dll": """
ImmDisableTextFrameService s 1 i
ImmGetCompositionStringW s 4 i
ImmSetCompositionStringW s 6 i
ImmNotifyIME s 4 i
ImmSetOpenStatus s 2 i
ImmSimulateHotKey s 2 i
ImmReleaseContext s 2 i
ImmGetConversionStatus s 3 i
ImmGetContext s 1 i
ImmGetOpenStatus s 1 i
ImmGetCandidateListW s 4 i
ImmSetConversionStatus s 3 i
""",
"KERNEL32.dll": """
GlobalMemoryStatusEx s 1 i
QueryPerformanceFrequency s 1 i
SetThreadPriority s 2 i
SetPriorityClass s 2 i
GetPriorityClass s 1 i
GetThreadPriority s 1 i
GetCurrentThread s 0 i
SetThreadAffinityMask s 2 i
GetProcessAffinityMask s 3 i
GetProcAddress s 2 i
GetModuleHandleA s 1 i
CloseHandle s 1 i
DeviceIoControl s 8 i
CreateFileA s 7 i
LocalFree s 1 i
LocalAlloc s 2 i
GetModuleFileNameA s 3 i
GetSystemInfo s 1 v
GetVersionExA s 1 i
FreeLibrary s 1 i
LoadLibraryA s 1 i
GetComputerNameExA s 3 i
CompareStringW s 6 i
LCMapStringW s 6 i
InterlockedIncrement s 1 i
InterlockedDecrement s 1 i
GetSystemDirectoryW s 2 i
GetVersionExW s 1 i
IsDebuggerPresent s 0 i
SetProcessAffinityMask s 2 i
OpenProcess s 3 i
GetCurrentProcessId s 0 i
SetThreadExecutionState s 1 i
GetLocaleInfoW s 4 i
SetLastError s 1 v
EnumResourceNamesA s 4 i
GetSystemDefaultLCID s 0 i
GetWindowsDirectoryW s 2 i
GetTickCount s 0 i
VirtualQuery s 3 i
ResumeThread s 1 i
GetCurrentThreadId s 0 i
CreateEventA s 4 i
FormatMessageA s 7 i
FlushFileBuffers s 1 i
TerminateProcess s 2 i
SetUnhandledExceptionFilter s 1 i
GetComputerNameA s 2 i
SystemTimeToFileTime s 2 i
GetSystemTime s 1 v
GetModuleFileNameW s 3 i
OutputDebugStringA s 1 v
GetExitCodeThread s 2 i
TlsAlloc s 0 i
TlsSetValue s 2 i
DuplicateHandle s 7 i
SetThreadIdealProcessor s 2 i
SleepEx s 2 i
CreateMutexA s 3 i
WaitForSingleObject s 2 i
TryEnterCriticalSection s 1 i
ReleaseMutex s 1 i
InterlockedExchangeAdd s 2 i
InterlockedCompareExchange s 3 i
CreateSemaphoreA s 4 i
ReleaseSemaphore s 3 i
InterlockedExchange s 2 i
TlsFree s 1 i
RaiseException s 4 v
QueueUserAPC s 3 i
CreateThread s 6 i
HeapAlloc s 3 i
GetProcessHeap s 0 i
HeapFree s 3 i
InitializeCriticalSectionAndSpinCount s 2 i
VirtualFree s 3 i
VirtualAlloc s 4 i
GetSystemTimeAsFileTime s 1 v
GlobalFree s 1 i
GlobalUnlock s 1 i
GlobalLock s 1 i
GlobalAlloc s 2 i
GlobalSize s 1 i
GetVersion s 0 i
GetModuleHandleW s 1 i
FindFirstFileW s 2 i
FindNextFileW s 2 i
FindClose s 1 i
SetEvent s 1 i
CancelIo s 1 i
GetLongPathNameW s 3 i
WaitForMultipleObjectsEx s 5 i
FindCloseChangeNotification s 1 i
FindNextChangeNotification s 1 i
WaitForMultipleObjects s 4 i
FindFirstChangeNotificationW s 3 i
CreateFileW s 7 i
GetVolumeInformationW s 8 i
MultiByteToWideChar s 6 i
WideCharToMultiByte s 8 i
GetLastError s 0 i
GetFileSize s 2 i
SetEndOfFile s 1 i
SetFilePointer s 4 i
ReadFile s 5 i
WriteFile s 5 i
GetFileAttributesW s 1 i
DeleteFileW s 1 i
MoveFileExW s 3 i
CopyFileW s 3 i
SetFileAttributesW s 2 i
GetTempPathW s 2 i
GetVolumePathNameW s 3 i
CreateDirectoryW s 2 i
GetFullPathNameW s 4 i
RemoveDirectoryW s 1 i
SetEnvironmentVariableA s 2 i
GetEnvironmentVariableA s 3 i
LoadLibraryW s 1 i
GetACP s 0 i
GetFullPathNameA s 4 i
GetProcessTimes s 5 i
GetCommandLineA s 0 i
Sleep s 1 v
MoveFileA s 2 i
CreateDirectoryA s 2 i
RemoveDirectoryA s 1 i
GetCurrentDirectoryA s 2 i
UnhandledExceptionFilter s 1 i
GetStartupInfoA s 1 v
FlushConsoleInputBuffer s 1 i
FindFirstFileA s 2 i
FindNextFileA s 2 i
GlobalMemoryStatus s 1 v
GetStdHandle s 1 i
GetFileType s 1 i
QueryPerformanceCounter s 1 i
TlsGetValue s 1 i
GetDiskFreeSpaceExA s 4 i
GetVolumeInformationA s 8 i
GetDriveTypeA s 1 i
GetLogicalDrives s 0 i
InitializeCriticalSection s 1 v
DeleteCriticalSection s 1 v
EnterCriticalSection s 1 v
LeaveCriticalSection s 1 v
DeleteFileA s 1 i
GetCurrentProcess s 0 i
""",
"MSVCR90.dll": """
tolower c 1 i
_controlfp_s c 3 i
_invoke_watson c 5 n
?_type_info_dtor_internal_method@type_info@@QAEXXZ t 0 v
_except_handler4_common c 6 i
_time32 c 1 i
__set_app_type c 1 v
__p__fmode c 0 i
__p__commode c 0 i
_adjust_fdiv d 0 i
__setusermatherr c 1 v
_configthreadlocale c 1 i
_initterm_e c 2 i
_initterm c 2 v
_acmdln d 0 i
_ismbblead c 1 i
_XcptFilter c 2 i
_exit c 1 n
_cexit c 0 v
__getmainargs c 5 i
_amsg_exit c 1 n
?terminate@@YAXXZ c 0 n
_decode_pointer c 1 i
_onexit c 1 i
_lock c 1 v
_encode_pointer c 1 i
__dllonexit c 3 i
_unlock c 1 v
_getch c 0 i
signal c 2 i
isupper c 1 i
strcmp c 2 i
vfprintf c 3 i
ldexp c 3 f
vsprintf c 3 i
_aligned_free c 1 v
_aligned_malloc c 2 i
wcsncat c 3 i
_control87 c 2 i
wcscspn c 2 i
strncat c 3 i
_controlfp c 2 i
_ultoa c 3 i
_strlwr c 1 i
fputs c 2 i
_isnan c 2 i
_finite c 2 i
_searchenv c 3 v
_wsearchenv c 3 v
_spawnv c 3 i
_wspawnv c 3 i
_strtoui64 c 3 l
_wgetcwd c 2 i
_wstat64i32 c 2 i
_getdrive c 0 i
_wgetdcwd c 3 i
_fcvt c 5 i
_ecvt c 5 i
_beginthreadex c 6 i
_mktime64 c 1 l
strftime c 4 i
_localtime64 c 1 i
strtol c 3 i
strpbrk c 2 i
isprint c 1 i
atof c 1 f
atoi c 1 i
towupper c 1 i
setlocale c 2 i
_setmbcp c 1 i
_wcslwr c 1 i
_ltow c 3 i
strrchr c 2 i
_gmtime64 c 1 i
abort c 0 n
_snprintf c 3+ i
ftell c 1 i
fflush c 1 i
_vsnprintf c 4 i
_errno c 0 i
ferror c 1 i
longjmp c 2 n
getenv c 1 i
tmpfile c 0 i
fseek c 3 i
_setjmp3 c 2+ i
_ltoa c 3 i
fgets c 3 i
_stat64i32 c 2 i
strspn c 2 i
strcspn c 2 i
wcsrchr c 2 i
__iob_func c 0 i
_aligned_realloc c 3 i
exit c 1 n
_purecall c 0 i
_CIcos x 0 u
_CIsin x 0 u
_CIsqrt x 0 u
memset c 3 i
memcpy c 3 i
memmove c 3 i
_CIasin x 0 u
_CIfmod x 0 b
_CIacos x 0 u
fclose c 1 i
fprintf c 2+ i
fopen c 2 i
fwrite c 4 i
sprintf c 2+ i
_CIpow x 0 b
wcstoul c 3 i
_ultow c 3 i
strtod c 2 f
_CIlog x 0 u
floor c 2 f
ceil c 2 f
modf c 3 f
__CxxFrameHandler3 c 4 i
atol c 1 i
isdigit c 1 i
_strnicmp c 3 i
_CxxThrowException s 2 n
sscanf c 2+ i
_stricmp c 2 i
swscanf c 2+ i
wcstol c 3 i
wcsncmp c 3 i
_wtol c 1 i
wcsncpy c 3 i
strncpy c 3 i
strtoul c 3 i
strncmp c 3 i
toupper c 1 i
strchr c 2 i
isspace c 1 i
_wcsicmp c 2 i
isalnum c 1 i
wcstod c 2 f
isalpha c 1 i
wcsstr c 2 i
wcschr c 2 i
_wcsnicmp c 3 i
towlower c 1 i
iswctype c 2 i
isxdigit c 1 i
memchr c 3 i
strstr c 2 i
printf c 1+ i
qsort c 4 v
_except_handler3 c 4 i
fread c 4 i
feof c 1 i
free c 1 v
realloc c 2 i
malloc c 1 i
_time64 c 1 l
rand c 0 i
_crt_debugger_hook c 1 v
""",
"PSAPI.DLL": """
GetModuleInformation s 4 i
""",
"SHELL32.dll": """
SHGetFolderPathW s 5 i
DragQueryFileW s 4 i
ShellExecuteW s 6 i
""",
"USER32.dll": """
SetWindowLongW s 3 i
CreateWindowExW s 12 i
SetWindowPos s 7 i
GetWindowLongA s 2 i
GetSystemMetrics s 1 i
GetClientRect s 2 i
SetWindowTextW s 2 i
DestroyWindow s 1 i
GetDesktopWindow s 0 i
IsDlgButtonChecked s 2 i
CheckDlgButton s 3 i
GetDoubleClickTime s 0 i
wsprintfA c 2+ i
RegisterClassA s 1 i
CreateWindowExA s 12 i
SetClipboardViewer s 1 i
IsClipboardFormatAvailable s 1 i
GetClipboardData s 1 i
TranslateMessage s 1 i
EmptyClipboard s 0 i
SetClipboardData s 2 i
CloseClipboard s 0 i
ChangeClipboardChain s 2 i
wsprintfW c 2+ i
GetCursorPos s 1 i
GetKeyboardState s 1 i
DefWindowProcA s 4 i
PeekMessageA s 5 i
GetKeyboardLayout s 1 i
RegisterClassExW s 1 i
SetTimer s 4 i
SetCursor s 1 i
SetCursorPos s 2 i
IntersectRect s 3 i
ReleaseCapture s 0 i
SetCapture s 1 i
GetMenu s 1 i
AdjustWindowRectEx s 4 i
MoveWindow s 6 i
BeginPaint s 2 i
EndPaint s 2 i
EnumDisplayMonitors s 4 i
PeekMessageW s 5 i
DispatchMessageW s 1 i
ClientToScreen s 2 i
KillTimer s 2 i
UnregisterClassA s 2 i
GetKeyState s 1 i
PostMessageA s 4 i
ShowCursor s 1 i
GetWindowRect s 2 i
MonitorFromWindow s 2 i
IsZoomed s 1 i
GetAncestor s 2 i
IsIconic s 1 i
IsWindowVisible s 1 i
SetActiveWindow s 1 i
GetActiveWindow s 0 i
FillRect s 3 i
UpdateWindow s 1 i
ValidateRect s 2 i
InvalidateRect s 3 i
LoadIconA s 2 i
DestroyCursor s 1 i
GetIconInfo s 2 i
LoadImageW s 6 i
UnhookWindowsHookEx s 1 i
SetWindowsHookExA s 4 i
GetAsyncKeyState s 1 i
CallNextHookEx s 4 i
GetForegroundWindow s 0 i
GetWindowThreadProcessId s 2 i
GetMonitorInfoA s 2 i
ScreenToClient s 2 i
SendMessageA s 4 i
LoadCursorA s 2 i
SetWindowLongA s 3 i
SetFocus s 1 i
DefWindowProcW s 4 i
GetDC s 1 i
ReleaseDC s 2 i
SystemParametersInfoA s 4 i
GetProcessWindowStation s 0 i
GetUserObjectInformationW s 5 i
ShowWindow s 2 i
MessageBoxW s 4 i
MessageBoxA s 4 i
OpenClipboard s 1 i
PostQuitMessage s 1 v
""",
"USP10.dll": """
ScriptItemize s 7 i
ScriptBreak s 4 i
""",
"VERSION.dll": """
GetFileVersionInfoSizeA s 2 i
GetFileVersionInfoA s 4 i
VerQueryValueA s 4 i
VerQueryValueW s 4 i
GetFileVersionInfoW s 4 i
""",
"WINMM.dll": """
timeEndPeriod s 1 i
timeBeginPeriod s 1 i
timeGetTime s 0 i
""",
"WS2_32.dll": """
WSAIoctl s 9 i
WSASetLastError s 1 v ord=112
WSASocketA s 6 i
""",
"WSOCK32.dll": """
send s 4 i ord=19
listen s 2 i ord=13
connect s 3 i ord=4
accept s 3 i ord=1
getsockname s 3 i ord=6
getsockopt s 5 i ord=7
__WSAFDIsSet s 2 i ord=151
select s 5 i ord=18
recv s 4 i ord=16
inet_addr s 1 i ord=10
gethostbyname s 1 i ord=52
closesocket s 1 i ord=3
WSAStartup s 2 i ord=115
WSACleanup s 0 i ord=116
WSAGetLastError s 0 i ord=111
sendto s 6 i ord=20
recvfrom s 6 i ord=17
ioctlsocket s 3 i ord=12
socket s 3 i ord=23
bind s 3 i ord=2
shutdown s 2 i ord=22
getpeername s 3 i ord=5
setsockopt s 5 i ord=21
""",
"d3d9.dll": """
Direct3DCreate9 s 1 i
""",
"d3dx9_27.dll": """
D3DXVec3Transform s 3 i
D3DXVec3TransformNormal s 3 i
D3DXCheckVersion s 2 i
D3DXGetPixelShaderProfile s 1 i
D3DXGetVertexShaderProfile s 1 i
D3DXMatrixMultiplyTranspose s 3 i
D3DXMatrixTranspose s 2 i
D3DXMatrixMultiply s 3 i
D3DXCompileShader s 10 i
""",
"dbghelp.dll": """
UnDecorateSymbolName s 4 i
SymLoadModule64 s 7 l
SymInitialize s 3 i
SymSetOptions s 1 i
SymCleanup s 1 i
SymGetSymFromAddr64 s 5 i
SymGetLineFromAddr64 s 5 i
""",
"ole32.dll": """
OleInitialize s 1 i
RegisterDragDrop s 2 i
RevokeDragDrop s 1 i
CoLockObjectExternal s 3 i
OleUninitialize s 0 v
ReleaseStgMedium s 1 v
""",
"steam_api.dll": """
SteamApps c 0 i
SteamAPI_Shutdown c 0 v
SteamAPI_Init c 0 i
""",
}

# Dynamically resolved names (GetProcAddress after LoadLibrary/GetModuleHandle), from
# data/imports.json "dynamic_apis". policy: provide = the runtime returns a thunk;
# absent = GetProcAddress (or the LoadLibrary before it) fails, as on a GOG install
# without that component; the reason is given in the generated table.
# Line format: <module> <name> <cc> <slots> <ret> <policy> [reason...]
DYNAMIC = """
xinput9_1_0.dll XInputGetState s 2 i provide gamepad (later: GameController)
xinput9_1_0.dll XInputSetState s 2 i provide gamepad rumble
xinput9_1_0.dll XInputGetCapabilities s 3 i provide gamepad
xinput9_1_0.dll XInputGetDSoundAudioDeviceGuids s 3 i provide headset audio; returns ERROR_DEVICE_NOT_CONNECTED
nvapi.dll nvapi_QueryInterface c 1 i absent no NVIDIA driver library
gdi32.dll D3DKMTOpenAdapterFromHdc s 1 i absent WDDM escape; not present (runtime-abi 7.2)
gdi32.dll D3DKMTEscape s 1 i absent WDDM escape; not present (runtime-abi 7.2)
user32.dll EnumDisplayDevicesA s 4 i provide GPU description query
user32.dll TrackMouseEvent s 1 i provide mouse-leave tracking
user32.dll GetForegroundWindow s 0 i provide OpenSSL RAND_poll entropy
user32.dll GetCursorInfo s 1 i provide OpenSSL RAND_poll entropy
user32.dll GetQueueStatus s 1 i provide OpenSSL RAND_poll entropy
ntdll TGGetOS c 0 i absent TransGaming Cider probe; NULL keeps $cider false (not running under Cider)
kernel32.dll GetProductInfo s 5 i provide OS edition for the system-info report
kernel32.dll ReadDirectoryChangesW s 8 i provide directory watching
kernel32.dll GetModuleFileNameW s 3 i provide also a static import
kernel32.dll CreateToolhelp32Snapshot s 2 i absent OpenSSL RAND_poll walk; it tolerates NULL
kernel32.dll CloseToolhelp32Snapshot s 1 i absent does not exist in real kernel32 either
kernel32.dll Heap32First s 3 i absent OpenSSL RAND_poll walk
kernel32.dll Heap32Next s 1 i absent OpenSSL RAND_poll walk
kernel32.dll Heap32ListFirst s 2 i absent OpenSSL RAND_poll walk
kernel32.dll Heap32ListNext s 2 i absent OpenSSL RAND_poll walk
kernel32.dll Process32First s 2 i absent OpenSSL RAND_poll walk
kernel32.dll Process32Next s 2 i absent OpenSSL RAND_poll walk
kernel32.dll Thread32First s 2 i absent OpenSSL RAND_poll walk
kernel32.dll Thread32Next s 2 i absent OpenSSL RAND_poll walk
kernel32.dll Module32First s 2 i absent OpenSSL RAND_poll walk
kernel32.dll Module32Next s 2 i absent OpenSSL RAND_poll walk
shell32.dll ShellExecuteW s 6 i provide also a static import
advapi32.dll CryptAcquireContextW s 5 i provide OpenSSL RAND_poll; backed by arc4random
advapi32.dll CryptGenRandom s 3 i provide OpenSSL RAND_poll
advapi32.dll CryptReleaseContext s 2 i provide OpenSSL RAND_poll
netapi32.dll NetStatisticsGet s 5 i absent LoadLibrary("NETAPI32.DLL") fails
netapi32.dll NetApiBufferFree s 1 i absent LoadLibrary("NETAPI32.DLL") fails
dbghelp.dll SymInitialize s 3 i provide crash handler; returns FALSE (native crash report instead)
dbghelp.dll SymCleanup s 1 i provide crash handler; returns FALSE
dbghelp.dll StackWalk s 9 i provide crash handler; returns FALSE
dbghelp.dll SymFunctionTableAccess s 2 i provide crash handler; returns NULL
dbghelp.dll SymGetModuleBase s 2 i provide crash handler; returns 0
dbghelp.dll SymGetSymFromAddr s 4 i provide crash handler; returns FALSE
dbghelp.dll SymGetLineFromAddr s 4 i provide crash handler; returns FALSE
dbghelp.dll MiniDumpWriteDump s 7 i provide crash handler; returns FALSE
dbghelp.dll SymGetOptions s 0 i provide crash handler
dbghelp.dll SymSetOptions s 1 i provide also a static import
dbghelp.dll StackWalk64 s 9 i provide crash handler; returns FALSE
dbghelp.dll SymFunctionTableAccess64 s 3 i provide crash handler; returns NULL
dbghelp.dll SymGetModuleBase64 s 3 l provide crash handler; returns 0
dbghelp.dll SymGetSymFromAddr64 s 5 i provide also a static import
dbghelp.dll SymGetLineFromAddr64 s 5 i provide also a static import
DasmX86Dll.dll DasmX86_init c 0 i absent optional crash-report disassembler; game prints 'DLL not found'
DasmX86Dll.dll DasmX86_cleanup c 0 i absent optional crash-report disassembler
DasmX86Dll.dll DasmX86_disasm c 0 i absent optional crash-report disassembler
DasmX86Dll.dll DasmX86_format_insn c 0 i absent optional crash-report disassembler
psapi.dll EnumProcessModules s 4 i provide crash/system report; one module
psapi.dll GetModuleFileNameExA s 4 i provide crash/system report
psapi.dll GetModuleBaseNameA s 4 i provide crash/system report
psapi.dll GetModuleInformation s 4 i provide also a static import
rasapi32.dll RasEnumConnectionsA s 3 i absent dial-up detection; LoadLibrary("rasapi32.dll") fails [inf: caller at 0x94c239 checks the handle]
rasapi32.dll RasEnumEntriesA s 5 i absent dial-up detection
rasapi32.dll RasGetProjectionInfoA s 4 i absent dial-up detection
rasapi32.dll RasGetEntryDialParamsA s 3 i absent dial-up detection
rasapi32.dll RasDialA s 6 i absent dial-up detection
rasapi32.dll RasGetConnectStatusA s 2 i absent dial-up detection
rasapi32.dll RasHangUpA s 1 i absent dial-up detection
plugin EASharedLibraryStartupFunction c 0 i absent EA plugin DLLs are not shipped; LoadLibraryW fails
plugin oaPluginInit c 0 i absent plugin DLLs are not shipped; LoadLibraryA fails
"""


def parse():
    """Returns (imports, dynamic): imports maps (dll, name) -> dict; dynamic is a list of dicts."""
    imp = {}
    for dll, block in SIGS.items():
        for line in block.strip().splitlines():
            p = line.split()
            name, cc, slots, ret = p[:4]
            flags = dict(f.split("=", 1) for f in p[4:])
            varargs = slots.endswith("+")
            imp[(dll, name)] = dict(dll=dll, name=name, cc=cc, slots=int(slots.rstrip("+")),
                                    varargs=varargs, ret=ret,
                                    ordinal=int(flags["ord"]) if "ord" in flags else None)
    dyn = []
    for line in DYNAMIC.strip().splitlines():
        p = line.split()
        mod, name, cc, slots, ret, policy = p[:6]
        dyn.append(dict(module=mod, name=name, cc=cc, slots=int(slots), varargs=False, ret=ret,
                        policy=policy, reason=" ".join(p[6:])))
    return imp, dyn
