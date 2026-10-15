"""Import table, step 4 of 4: merge the output of parse_imports.py, callsites.py and dynres.py into
data/imports.json, with layer tags.

The "com_surface" and "dynamic_apis" sections are analysts' work (COM vtable-call counts named by hand, dynamic
GetProcAddress sites read by hand), kept in data/imports-hand.json; --hand-sections copies them unchanged from the
given file (any file with those keys, an older imports.json for example). Everything else is generated, byte for
byte reproducibly, and git-ignored.

Usage:   python3 -P tools/imports/build_table.py imp.json cs.json dyn.json OUT.json [--hand-sections data/imports-hand.json]
         The whole chain, from the top of the source tree with EXE=game/SporebinEP1/SporeApp.exe:
           python3 -P tools/imports/parse_imports.py $EXE > imp.json
           PYTHONPATH=tools/.deps python3 -P tools/imports/callsites.py $EXE imp.json > cs.json
           PYTHONPATH=tools/.deps python3 -P tools/imports/dynres.py $EXE cs.json > dyn.json
           python3 -P tools/imports/build_table.py imp.json cs.json dyn.json data/imports.json \
               --hand-sections data/imports-hand.json
Inputs:  the three JSON files above and the hand-made sections
Outputs: OUT.json (data/imports.json), read by tools/threads/scan.py and tools/hle/gen_thunks.py
When:    the converter's imports stage (see parse_imports.py).
"""
import json, re, sys, collections

args = sys.argv[1:]
hand = None
if "--hand-sections" in args:
    i = args.index("--hand-sections"); hand = json.load(open(args[i + 1])); del args[i:i + 2]
imp, cs, dyn, outp = json.load(open(args[0])), json.load(open(args[1])), json.load(open(args[2])), args[3]

# Standard Winsock 1.1 / DirectSound export ordinals (Wine .spec files / MS docs).
WSOCK = {1:"accept",2:"bind",3:"closesocket",4:"connect",5:"getpeername",6:"getsockname",7:"getsockopt",
         8:"htonl",9:"htons",10:"inet_addr",11:"inet_ntoa",12:"ioctlsocket",13:"listen",14:"ntohl",15:"ntohs",
         16:"recv",17:"recvfrom",18:"select",19:"send",20:"sendto",21:"setsockopt",22:"shutdown",23:"socket",
         51:"gethostbyaddr",52:"gethostbyname",53:"getprotobyname",57:"gethostname",111:"WSAGetLastError",
         112:"WSASetLastError",115:"WSAStartup",116:"WSACleanup",151:"__WSAFDIsSet"}
DSOUND = {1:"DirectSoundCreate",2:"DirectSoundEnumerateA",3:"DirectSoundEnumerateW",6:"DirectSoundCaptureCreate",
          7:"DirectSoundCaptureEnumerateA",8:"DirectSoundCaptureEnumerateW",9:"GetDeviceID",11:"DirectSoundCreate8"}

def layer(dll, n):
    dl = dll.lower()
    if dl == "msvcr90.dll":
        if re.match(r"(_CI|floor|ceil|modf|ldexp|_isnan|_finite|_control|_fcvt|_ecvt|__setusermatherr|_adjust_fdiv)", n): return "crt-math"
        if re.match(r"(memcpy|memset|memmove|memchr)$", n): return "crt-memory"
        if re.match(r"(malloc|free|realloc|_aligned_)", n): return "crt-heap"
        if re.match(r"(f(open|close|read|write|seek|tell|flush|puts|gets|printf)|feof|ferror|tmpfile|__iob_func|_getch|printf|vfprintf|_w?stat64i32|_wgetcwd|_wgetdcwd|_getdrive|_w?searchenv)", n): return "crt-stdio-fs"
        if re.match(r"(_time|_mktime|_localtime|_gmtime|strftime|rand)", n): return "crt-time-misc"
        if re.match(r"(_beginthreadex|_lock|_unlock|_configthreadlocale)", n): return "crt-threads"
        if re.match(r"(__CxxFrameHandler3|_CxxThrowException|_except_handler|_XcptFilter|\?terminate|\?_type_info|_purecall|_invoke_watson|_crt_debugger_hook|signal|abort|_setjmp3|longjmp|_encode_pointer|_decode_pointer)", n): return "crt-eh-runtime"
        if re.match(r"(__set_app_type|__p__|_initterm|_acmdln|_exit|_cexit|exit|__getmainargs|_amsg_exit|_onexit|__dllonexit|getenv|_spawnv|_wspawnv)", n): return "crt-startup-process"
        if re.match(r"(setlocale|_setmbcp|_ismbblead)", n): return "crt-locale"
        return "crt-string"
    if dl in ("d3d9.dll", "d3dx9_27.dll"): return "graphics-d3d9"
    if dl in ("dsound.dll",): return "audio"
    if dl in ("winmm.dll",): return "time"
    if dl in ("dinput8.dll",): return "input"
    if dl in ("imm32.dll", "usp10.dll"): return "text-ime"
    if dl in ("wsock32.dll", "ws2_32.dll"): return "networking"
    if dl in ("dbghelp.dll", "psapi.dll"): return "debug-diagnostics"
    if dl == "steam_api.dll": return "platform-store"
    if dl == "version.dll": return "system-info"
    if dl == "ole32.dll": return "com-ole-dragdrop"
    if dl == "shell32.dll": return "shell"
    if dl == "gdi32.dll": return "window-gdi"
    if dl == "advapi32.dll":
        return "registry" if n.startswith("Reg") else ("debug-diagnostics" if "Event" in n else "system-info")
    if dl == "user32.dll":
        if re.search(r"Clipboard", n): return "clipboard"
        if re.search(r"Key|Cursor|Capture|Hook|DoubleClick|Focus", n): return "input"
        if n.startswith("wsprintf"): return "crt-string"
        return "window-message"
    if dl == "kernel32.dll":
        if re.search(r"Interlocked|CriticalSection|Mutex|Semaphore|Event$|Event[AW]|SetEvent|WaitFor|Thread|Tls|QueueUserAPC|DuplicateHandle|SleepEx|^Sleep$|ResumeThread|GetExitCode", n): return "threads-sync"
        if re.search(r"Heap|Virtual|Global(Alloc|Free|Lock|Unlock|Size)|Local(Alloc|Free)|MemoryStatus", n): return "memory"
        if re.search(r"File|Directory|Path|Volume|Drive|FindFirst|FindNext|FindClose|ChangeNotification|CancelIo|DeviceIoControl|DiskFree|SetEndOfFile|ReadFile|WriteFile|Logical|GetStdHandle|FlushConsole|CloseHandle", n): return "file-io"
        if re.search(r"PerformanceCounter|PerformanceFrequency|TickCount|SystemTime|ProcessTimes", n): return "time"
        if re.search(r"LoadLibrary|FreeLibrary|GetProcAddress|GetModuleHandle|GetModuleFileName|EnumResourceNames", n): return "dynamic-loading"
        if re.search(r"MultiByte|WideChar|CompareString|LCMapString|Locale|LCID|GetACP", n): return "locale-text"
        if re.search(r"Debugger|OutputDebugString|Exception|FormatMessage|TerminateProcess|RaiseException", n): return "debug-diagnostics"
        return "process-system-info"
    return "other"

dynmap = {}
for e in dyn:
    dynmap.setdefault(e["api"], []).append(e)

rows = []
for e in cs["imports"]:
    n = e["name"]; inferred = None
    if n is None:
        tab = WSOCK if e["dll"].lower() in ("wsock32.dll", "ws2_32.dll") else DSOUND if e["dll"].lower() == "dsound.dll" else {}
        inferred = tab.get(e["ordinal"])
    nm = n or inferred or f"#{e['ordinal']}"
    rows.append(dict(dll=e["dll"], name=n, ordinal=e["ordinal"], ordinal_name_inferred=inferred,
                     iat=e["iat"], layer=layer(e["dll"], nm), uses=e["uses"], iat_refs=e["iat_refs"],
                     detail=e["detail"], sites=e["sites"]))

summary = collections.defaultdict(lambda: dict(imports=0, uses=0))
for r in rows:
    summary[r["layer"]]["imports"] += 1; summary[r["layer"]]["uses"] += r["uses"]
dlls = collections.defaultdict(lambda: dict(imports=0, uses=0))
for r in rows:
    dlls[r["dll"]]["imports"] += 1; dlls[r["dll"]]["uses"] += r["uses"]

out = dict(
    target=dict(file="SporebinEP1/SporeApp.exe",
                sha256="dc04aee5a3debc3f1ad4c1a937460e99a29b9bd3bc285008be83615dd5e59a37",
                image_base="0x400000", iat_range=["0x13cc000", "0x13cc80c"],
                note="Base-game SporeBin/SporeApp.exe imports the identical 495-function set."),
    method=("Static import directory parsed by tools/imports/parse_imports.py (no delay-load directory present). "
            "Uses counted by tools/imports/callsites.py from HIGHLOW relocations that point into the IAT, decoded "
            "with Capstone: call [IAT]; mov reg,[IAT] followed by call reg (linear, stops at ret/overwrite); "
            "jmp [IAT] thunks counted via E8/E9 rel32 callers and relocated pointers to the thunk (e.g. vtables). "
            "Dynamic resolution from tools/imports/dynres.py plus manual review."),
    totals=dict(dlls=len(dlls), imports=len(rows), uses=sum(r["uses"] for r in rows)),
    by_dll=dlls, by_layer=dict(sorted(summary.items(), key=lambda kv: -kv[1]["imports"])),
    imports=rows,
    dynamic_raw=dyn,
)
if hand is not None:
    for k in ("com_surface", "dynamic_apis"):
        if k in hand:
            out[k] = hand[k]
json.dump(out, open(outp, "w"), indent=1)
print(json.dumps(out["totals"]), json.dumps(out["by_layer"]), json.dumps(out["by_dll"]))
