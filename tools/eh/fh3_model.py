"""Executable reference model of the exception-handling runtime (runtime/core/src/core_eh.c;
docs/design/runtime-abi.md 10).

This is a specification written as Python, not a runtime. It models:
  * a toy 32-bit guest memory with a guest stack and a TEB whose dword 0 is the fs:[0] head;
  * "translated functions" as Python callables that operate on the guest context exactly the way
    generated C would (build their fs:[0] registration with plain stores, write the state slot);
  * host-level landings (__builtin_setjmp/__builtin_longjmp in the real runtime) as Python
    exceptions caught by the landing owner's driver loop, plus host cleanup records;
  * the runtime's native implementations of RaiseException-style dispatch (sg_seh_raise),
    global unwind (RtlUnwind), __CxxFrameHandler3 (MSVC FH3, magic 0x19930522),
    _except_handler3, _except_handler4_common, _CxxThrowException (throw and rethrow),
    _setjmp3 and longjmp.

Everything runs on fixtures this file builds itself (hand-made FuncInfo, scope tables, ThrowInfo);
no game bytes are used or executed. tools/eh/test_fh3_model.py exercises it.

Usage:   a library, imported by test_fh3_model.py; read it as the specification of runtime/core's EH code.

Guest structures follow the documented MSVC x86 layouts (field names as in ehdata.h / excpt.h):
  EHRegistrationNode (FH3)    next, handler, state            frame ebp = rn + 0x0C, saved esp at rn - 4
  EH3/EH4 registration        next, handler, scopetable, trylevel   ebp = rn + 0x10,
                              xpointers at rn - 4, saved esp at rn - 8
  FuncInfo                    magic, maxState, pUnwindMap, nTryBlocks, pTryBlockMap, nIPMapEntries,
                              pIPtoStateMap, pESTypeList, EHFlags
  UnwindMapEntry              toState, action
  TryBlockMapEntry            tryLow, tryHigh, catchHigh, nCatches, pHandlerArray
  HandlerType                 adjectives, pType, dispCatchObj, addressOfHandler
  ThrowInfo                   attributes, pmfnUnwind, pForwardCompat, pCatchableTypeArray
  CatchableTypeArray          nCatchableTypes, CatchableType*[]
  CatchableType               properties, pType, PMD{mdisp, pdisp, vdisp}, sizeOrOffset, copyFunction
  TypeDescriptor              pVFTable, spare, name[]
  _JUMP_BUFFER (setjmp3)      Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel, Cookie, UnwindFunc,
                              UnwindData[6]
"""
import struct

# ---------------------------------------------------------------------------- constants
EH_MAGIC = 0x19930522
EH_EXCEPTION_NUMBER = 0xE06D7363          # 'msc' | 0xE0000000
EH_MAGIC_NUMBER1 = 0x19930520             # ExceptionInformation[0] of a C++ exception
EXCEPTION_NONCONTINUABLE = 0x1
EXCEPTION_UNWINDING = 0x2
EXCEPTION_EXIT_UNWIND = 0x4
STATUS_LONGJUMP = 0x80000026
ExceptionContinueExecution, ExceptionContinueSearch = 0, 1
EXCEPTION_EXECUTE_HANDLER, EXCEPTION_CONTINUE_SEARCH, EXCEPTION_CONTINUE_EXECUTION = 1, 0, -1
HT_IsConst, HT_IsVolatile, HT_IsUnaligned, HT_IsReference = 1, 2, 4, 8
CT_IsSimpleType = 1
END_OF_CHAIN = 0xFFFFFFFF
VC20 = 0x56433230

MEM_SIZE = 0x200000
TEB = 0x1000
STACK_TOP = 0x1F0000
DATA = 0x20000


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


class Unhandled(Exception):
    """The fs:[0] chain ended without a handler: the real runtime logs and aborts (doc section 7)."""


class HostResume(BaseException):
    """Models __builtin_longjmp to a landing (sg_eh_resume)."""

    def __init__(self, landing):
        super().__init__()
        self.landing = landing


class BadResume(Exception):
    pass


class Landing:
    def __init__(self, func, frame_esp):
        self.func = func
        self.frame_esp = frame_esp


class Cleanup:
    """A host cleanup record: a runtime frame that holds host state while guest code runs above it."""

    def __init__(self, fn):
        self.fn = fn


class Guest:
    """Toy guest: memory, context, code table, the runtime's EH natives."""

    def __init__(self):
        self.mem = bytearray(MEM_SIZE)
        self.ctx = dict(eax=0, ebx=0, ecx=0, edx=0, esi=0, edi=0, ebp=0, esp=STACK_TOP, eip=0)
        self.code = {}          # guest address -> python callable(g)
        self.owner_of = {}      # resume address -> owning translated function (from data/eh resume table)
        self.host = []          # host landing/cleanup stack (ctx->eh_top), innermost last
        self.log = []           # observable events, for tests
        self.heap = DATA
        self.next_code = 0x401000
        self.in_flight = []     # C++ exceptions currently being handled: [(obj, throwinfo)]
        self.dispatch_obj = None  # object carried by the dispatch in progress (rethrow detection)
        self.w32(TEB, END_OF_CHAIN)
        # native entry points (the import thunks in the real image)
        self.FH3 = self.native(self.cxx_frame_handler3_entry, "__CxxFrameHandler3")
        self.EH3 = self.native(self.except_handler3, "_except_handler3")
        self.EH4 = self.native(self.except_handler4, "_except_handler4")
        self.CATCHGUARD = self.native(self.catch_guard_handler, "CatchGuardHandler")
        self.guards = {}
        self.cookie = 0x5A5A1234

    # ---------------------------------------------------------------- memory and stack
    def r32(self, a):
        return struct.unpack_from("<I", self.mem, a)[0]

    def w32(self, a, v):
        struct.pack_into("<I", self.mem, a, v & 0xFFFFFFFF)

    def alloc(self, n):
        a = self.heap
        self.heap += (n + 15) & ~15
        return a

    def words(self, *ws):
        a = self.alloc(4 * len(ws))
        for i, w in enumerate(ws):
            self.w32(a + 4 * i, w)
        return a

    def cstr(self, s):
        b = s.encode() + b"\0"
        a = self.alloc(len(b))
        self.mem[a:a + len(b)] = b
        return a

    def push(self, v):
        self.ctx["esp"] = (self.ctx["esp"] - 4) & 0xFFFFFFFF
        self.w32(self.ctx["esp"], v)

    def pop(self):
        v = self.r32(self.ctx["esp"])
        self.ctx["esp"] += 4
        return v

    @property
    def fs0(self):
        return self.r32(TEB)

    @fs0.setter
    def fs0(self, v):
        self.w32(TEB, v)

    # ---------------------------------------------------------------- code
    def native(self, fn, name=None):
        a = self.next_code
        self.next_code += 0x10
        self.code[a] = fn
        return a

    def call(self, addr, *args):
        """Guest cdecl/stdcall-agnostic call used by the model: push args, push a return address,
        run the function; the callee pops its own return address (and args if stdcall)."""
        for a in reversed(args):
            self.push(a)
        esp_before = self.ctx["esp"]
        self.push(0xCA11BAC0)
        self.code[addr](self)
        # cdecl: caller pops args; we normalise so tests do not depend on the convention
        self.ctx["esp"] = esp_before + 4 * len(args)
        return self.ctx["eax"]

    def ret(self):
        self.pop()

    def call_funclet(self, addr, frame_ebp):
        """_CallSettingFrame: run a funclet with ebp set to the parent frame; return its eax."""
        saved = self.ctx["ebp"]
        self.ctx["ebp"] = frame_ebp
        self.push(0xF0C1E7)
        self.code[addr](self)
        self.ctx["ebp"] = saved
        return self.ctx["eax"]

    # ---------------------------------------------------------------- landings
    def run_landing(self, func, body, labels):
        """SG_LANDING_BEGIN + SG_RESUME_DISPATCH for a translated function that owns resume points.
        body() is the straight-line code; labels maps resume address -> continuation callable."""
        land = Landing(func, self.ctx["esp"] + 4)    # frame_esp: esp at entry (above the return address)
        self.host.append(land)
        nxt = body
        try:
            while True:
                try:
                    return nxt()
                except HostResume as r:
                    if r.landing is not land:
                        raise
                    eip = self.ctx["eip"]
                    if eip not in labels:
                        raise BadResume(hex(eip))
                    nxt = labels[eip]
        finally:
            # SG_LANDING_RET: unlink on any exit path that leaves this host frame
            if self.host and self.host[-1] is land:
                self.host.pop()

    def eh_resume(self, target):
        """sg_eh_resume: ctx already holds the resumed register state; jump to target."""
        func = self.owner_of.get(target)
        if func is None:
            raise BadResume("no landing owner for 0x%x" % target)
        esp = self.ctx["esp"]
        for i in range(len(self.host) - 1, -1, -1):
            l = self.host[i]
            if isinstance(l, Landing) and l.func == func and l.frame_esp > esp:
                # unlink inner records, running host cleanups innermost first
                for inner in reversed(self.host[i + 1:]):
                    if isinstance(inner, Cleanup):
                        inner.fn()
                del self.host[i + 1:]
                self.ctx["eip"] = target
                raise HostResume(l)
        raise BadResume("no live landing for 0x%x (func 0x%x)" % (target, func))

    # ---------------------------------------------------------------- dispatch (RtlDispatchException)
    def make_record(self, code, flags, params):
        rec = self.alloc(80)
        self.w32(rec + 0, code)
        self.w32(rec + 4, flags)
        self.w32(rec + 8, 0)
        self.w32(rec + 12, 0)
        self.w32(rec + 16, len(params))
        for i, p in enumerate(params):
            self.w32(rec + 20 + 4 * i, p)
        return rec

    def call_handler(self, rn, rec, flags=None):
        if flags is not None:
            self.w32(rec + 4, flags)
        handler = self.r32(rn + 4)
        ctxrec = 0   # CONTEXT is not modelled; the real runtime builds one on the guest stack
        return self.call(handler, rec, rn, ctxrec, 0)

    def seh_raise(self, rec):
        rn = self.fs0
        while rn != END_OF_CHAIN:
            r = self.call_handler(rn, rec)
            if r == ExceptionContinueExecution:
                if self.r32(rec + 4) & EXCEPTION_NONCONTINUABLE:
                    raise Unhandled("continue on noncontinuable 0x%x" % self.r32(rec))
                return
            rn = self.r32(rn)
        self.log.append(("unhandled", self.r32(rec)))
        raise Unhandled(hex(self.r32(rec)))

    def global_unwind(self, target_rn, rec):
        """RtlUnwind/_global_unwind2: call every handler above target_rn with the unwind flag."""
        flags = self.r32(rec + 4) | EXCEPTION_UNWINDING | (0 if target_rn != END_OF_CHAIN else EXCEPTION_EXIT_UNWIND)
        rn = self.fs0
        while rn != target_rn and rn != END_OF_CHAIN:
            self.call_handler(rn, rec, flags)
            rn = self.r32(rn)
            self.fs0 = rn
        self.w32(rec + 4, self.r32(rec + 4) & ~EXCEPTION_UNWINDING)

    # ---------------------------------------------------------------- C++ throw
    def cxx_throw(self, obj, throwinfo):
        """_CxxThrowException(pObject, pThrowInfo). obj == throwinfo == 0 is `throw;`."""
        if obj == 0 and throwinfo == 0:
            if not self.in_flight:
                raise Unhandled("rethrow with no current exception (terminate)")
            obj, throwinfo = self.in_flight[-1]
            self.log.append(("rethrow", obj))
        rec = self.make_record(EH_EXCEPTION_NUMBER, EXCEPTION_NONCONTINUABLE, [EH_MAGIC_NUMBER1, obj, throwinfo])
        self.dispatch_obj = obj
        self.seh_raise(rec)
        raise AssertionError("C++ exception dispatch returned")

    # ---------------------------------------------------------------- __CxxFrameHandler3
    def cxx_frame_handler3_entry(self, g):
        """Entered from a guest stub `mov eax, FuncInfo; jmp __CxxFrameHandler3`: FuncInfo in eax,
        cdecl args (pExcept, pRN, pContext, pDC) on the guest stack."""
        esp = self.ctx["esp"]
        rec, rn = self.r32(esp + 4), self.r32(esp + 8)
        fi = self.ctx["eax"]
        self.ctx["eax"] = self.frame_handler3(rec, rn, fi)
        self.ret()

    def fi_fields(self, fi):
        magic, maxst, punw, ntry, ptry = [self.r32(fi + 4 * i) for i in range(5)]
        assert magic == EH_MAGIC
        return maxst, punw, ntry, ptry

    def frame_handler3(self, rec, rn, fi):
        maxst, punw, ntry, ptry = self.fi_fields(fi)
        flags = self.r32(rec + 4)
        if flags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND):
            if maxst and s32(self.r32(rn + 8)) != -1:
                self.unwind_to_state(rn, fi, -1)
            return ExceptionContinueSearch
        if ntry == 0 or self.r32(rec) != EH_EXCEPTION_NUMBER:
            # Not a C++ exception (or no try blocks). FH3 also consults translators for SEH
            # exceptions; with no _set_se_translator in this exe nothing more happens.
            return ExceptionContinueSearch
        obj, ti = self.r32(rec + 24), self.r32(rec + 28)
        state = s32(self.r32(rn + 8))
        for t in range(ntry):
            lo, hi, chi, ncatch, parr = [s32(self.r32(ptry + 20 * t + 4 * k)) for k in range(5)]
            if not (lo <= state <= hi):
                continue
            for c in range(ncatch):
                ht = (parr & 0xFFFFFFFF) + 16 * c
                ct = self.match(ht, ti)
                if ct is None:
                    continue
                self.catch_it(rec, rn, fi, ht, ct, obj, ti, lo, hi)   # does not return
        return ExceptionContinueSearch

    def type_name(self, td):
        a = td + 8
        e = self.mem.index(b"\0", a)
        return bytes(self.mem[a:e])

    def match(self, ht, ti):
        """Return the CatchableType that matches HandlerType ht, or None (TypeMatch in frame.cpp)."""
        adj, ptype = self.r32(ht), self.r32(ht + 4)
        cta = self.r32(ti + 12)
        n = self.r32(cta)
        for k in range(n):
            ct = self.r32(cta + 4 + 4 * k)
            if ptype == 0:                                   # catch (...)
                return ct
            cptype = self.r32(ct + 4)
            if cptype != ptype and self.type_name(cptype) != self.type_name(ptype):
                continue
            props = self.r32(ct)
            tiattr = self.r32(ti)
            # a reference-only CatchableType (CT_ByReferenceOnly = 2) needs a reference handler
            if props & 2 and not adj & HT_IsReference:
                continue
            # const/volatile of the thrown object must be accepted by the handler
            if tiattr & 1 and not adj & HT_IsConst:
                continue
            if tiattr & 2 and not adj & HT_IsVolatile:
                continue
            return ct
        return None

    def adjust_this(self, obj, ct):
        mdisp, pdisp, vdisp = s32(self.r32(ct + 8)), s32(self.r32(ct + 12)), s32(self.r32(ct + 16))
        p = obj + mdisp
        if pdisp >= 0:
            vbtable = self.r32(obj + pdisp)
            p += s32(self.r32(vbtable + vdisp)) + pdisp
        return p & 0xFFFFFFFF

    def build_catch_object(self, rn, ht, ct, obj):
        adj, ptype, disp = self.r32(ht), self.r32(ht + 4), s32(self.r32(ht + 8))
        if ptype == 0 or disp == 0:
            return
        dest = (rn + 12 + disp) & 0xFFFFFFFF
        src = self.adjust_this(obj, ct)
        if adj & HT_IsReference:
            self.w32(dest, src)
        elif self.r32(ct) & CT_IsSimpleType:
            n = self.r32(ct + 20)
            self.mem[dest:dest + n] = self.mem[src:src + n]
        else:
            copy = self.r32(ct + 24)
            if copy:
                self.ctx["ecx"] = dest                         # thiscall copy constructor
                self.call(copy, src)
            else:
                n = self.r32(ct + 20)
                self.mem[dest:dest + n] = self.mem[src:src + n]

    def unwind_to_state(self, rn, fi, target):
        """__FrameUnwindToState: run unwind actions from the current state down to target."""
        maxst, punw, _, _ = self.fi_fields(fi)
        cur = s32(self.r32(rn + 8))
        while cur != target and cur != -1:
            assert 0 <= cur < maxst, "state %d out of range" % cur
            to, act = s32(self.r32(punw + 8 * cur)), self.r32(punw + 8 * cur + 4)
            self.w32(rn + 8, to)            # set the state first, as MSVC does
            if act:
                self.call_funclet(act, rn + 12)
            cur = to
        self.w32(rn + 8, target)

    def destroy_object(self, obj, ti):
        dtor = self.r32(ti + 4)
        if dtor:
            self.ctx["ecx"] = obj
            self.call(dtor)

    def catch_it(self, rec, rn, fi, ht, ct, obj, ti, lo, hi):
        # 1. catch object, while the thrown object is still alive on the guest stack below us
        self.build_catch_object(rn, ht, ct, obj)
        # 2. global unwind of every frame above the catching one (destructors, __finally blocks)
        self.global_unwind(rn, rec)
        # 3. local unwind of the catching frame to the try block's low state
        self.unwind_to_state(rn, fi, lo)
        # 4. the catch block runs in state tryHigh + 1
        self.w32(rn + 8, hi + 1)
        self.in_flight.append((obj, ti))
        done = [False]

        def cleanup():
            # The catch block is being left by a new exception or a longjmp. Destroy the caught
            # object unless that exception *is* this object being rethrown.
            if done[0]:
                return
            done[0] = True
            self.in_flight.remove((obj, ti))
            if self.dispatch_obj != obj:
                self.destroy_object(obj, ti)
                self.log.append(("destroyed", obj))

        # The catch guard: a registration on top of the catching frame while the catch block runs
        # (MSVC's CatchGuardHandler). A new exception or a longjmp that leaves the catch block
        # unwinds through it, which destroys the caught object at the same point MSVC does.
        g = self
        g.push(rn)
        g.push(self.CATCHGUARD)
        g.push(self.fs0)
        guard = self.ctx["esp"]
        self.fs0 = guard
        self.guards[guard] = cleanup
        # The host frame of this function is also discarded in that case: a host cleanup record
        # drops the host-side bookkeeping (in the real runtime: nothing else is held here).
        rec_obj = Cleanup(lambda: self.guards.pop(guard, None))
        self.host.append(rec_obj)
        cont = self.call_funclet(self.r32(ht + 12), rn + 12)
        self.host.remove(rec_obj)
        self.guards.pop(guard, None)
        self.fs0 = self.r32(guard)
        done[0] = True
        self.in_flight.remove((obj, ti))
        self.destroy_object(obj, ti)
        self.log.append(("destroyed", obj))
        # 5. _JumpToContinuation
        self.ctx["esp"] = self.r32(rn - 4)
        self.ctx["ebp"] = rn + 12
        self.fs0 = rn
        self.eh_resume(cont)

    def catch_guard_handler(self, g):
        esp = self.ctx["esp"]
        rec, rn = self.r32(esp + 4), self.r32(esp + 8)
        if self.r32(rec + 4) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND):
            fn = self.guards.pop(rn, None)
            if fn:
                fn()
        # search phase: the catching frame's own registration is next in the chain and is searched
        # there (its state is now in the catch range), so the guard itself only continues the search
        self.ctx["eax"] = ExceptionContinueSearch
        self.ret()

    # ---------------------------------------------------------------- SEH: _except_handler3 / 4
    def except_handler3(self, g):
        esp = self.ctx["esp"]
        rec, rn = self.r32(esp + 4), self.r32(esp + 8)
        self.ctx["eax"] = self.seh_frame(rec, rn, self.r32(rn + 8), 0, -1)
        self.ret()

    def except_handler4(self, g):
        esp = self.ctx["esp"]
        rec, rn = self.r32(esp + 4), self.r32(esp + 8)
        st = self.r32(rn + 8) ^ self.cookie     # the frame stores scopetable XOR __security_cookie
        # (the GS/EH cookie checks named in the scope table header are done here by the real runtime)
        self.ctx["eax"] = self.seh_frame(rec, rn, st, 16, -2)
        self.ret()

    def scope(self, st, first, level):
        a = st + first + 12 * level
        return s32(self.r32(a)), self.r32(a + 4), self.r32(a + 8)

    def local_unwind_seh(self, rn, st, first, outside, stop):
        """_local_unwind2 / _local_unwind4: run __finally handlers from the current level down to stop."""
        lvl = s32(self.r32(rn + 12))
        while lvl != stop and lvl != outside and lvl != -1:
            enc, filt, hand = self.scope(st, first, lvl)
            self.w32(rn + 12, enc)
            if filt == 0:
                self.call_funclet(hand, rn + 0x10)
            lvl = enc

    def seh_frame(self, rec, rn, st, first, outside):
        ebp = rn + 0x10
        flags = self.r32(rec + 4)
        if flags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND):
            self.local_unwind_seh(rn, st, first, outside, outside)
            return ExceptionContinueSearch
        xp = self.words(rec, 0)                      # EXCEPTION_POINTERS {rec, context}
        self.w32(rn - 4, xp)
        lvl = s32(self.r32(rn + 12))
        while lvl != outside and lvl != -1:
            enc, filt, hand = self.scope(st, first, lvl)
            if filt:
                r = s32(self.call_funclet(filt, ebp))
                if r < 0:
                    return ExceptionContinueExecution
                if r > 0:
                    self.global_unwind(rn, rec)
                    self.local_unwind_seh(rn, st, first, outside, lvl)
                    self.w32(rn + 12, enc)
                    self.ctx["ebp"] = ebp
                    self.ctx["esp"] = self.r32(rn - 8)
                    self.fs0 = rn
                    self.eh_resume(hand)
            lvl = enc
        return ExceptionContinueSearch

    # ---------------------------------------------------------------- setjmp / longjmp
    def setjmp3(self, buf, ret_addr):
        """_setjmp3(buf, 0): the model takes the return address explicitly."""
        c = self.ctx
        for i, r in enumerate(("ebp", "ebx", "edi", "esi")):
            self.w32(buf + 4 * i, c[r])
        self.w32(buf + 16, c["esp"])
        self.w32(buf + 20, ret_addr)
        reg = self.fs0
        self.w32(buf + 24, reg)
        self.w32(buf + 28, 0xFFFFFFFF if reg == END_OF_CHAIN else self.r32(reg + 12))
        self.w32(buf + 32, 0)            # count 0: no cookie, no UnwindFunc (all 28 sites in this exe)
        c["eax"] = 0
        return 0

    def raise_exception(self, code, flags=0, params=()):
        """RaiseException for codes other than 0x406D1388 (which the real shim swallows)."""
        self.dispatch_obj = None
        self.seh_raise(self.make_record(code, flags, list(params)))

    def longjmp(self, buf, val):
        self.dispatch_obj = None
        reg = self.r32(buf + 24)
        if reg != self.fs0:
            rec = self.make_record(STATUS_LONGJUMP, 0, [buf])
            self.global_unwind(reg, rec)
        if reg != END_OF_CHAIN and self.r32(reg + 4) in (self.EH3,):
            self.local_unwind_seh(reg, self.r32(reg + 8), 0, -1, s32(self.r32(buf + 28)))
        c = self.ctx
        for i, r in enumerate(("ebp", "ebx", "edi", "esi")):
            c[r] = self.r32(buf + 4 * i)
        c["esp"] = self.r32(buf + 16)
        c["eax"] = val if val else 1
        self.fs0 = reg
        self.eh_resume(self.r32(buf + 20))


# ---------------------------------------------------------------------------- table builders
class Tables:
    """Helpers that lay out MSVC EH tables in the toy guest memory (our own fixtures)."""

    def __init__(self, g):
        self.g = g

    def typedesc(self, name):
        g = self.g
        b = name.encode() + b"\0"
        a = g.alloc(8 + len(b))
        g.w32(a, 0x7E1F0000)
        g.mem[a + 8:a + 8 + len(b)] = b
        return a

    def throwinfo(self, types, dtor=0, size=8, copy=0, attributes=0):
        """types: list of TypeDescriptor addresses, most derived first."""
        g = self.g
        cts = []
        for td in types:
            cts.append(g.words(0, td, 0, 0xFFFFFFFF, 0, size, copy))
        cta = g.words(len(cts), *cts)
        return g.words(attributes, dtor, 0, cta)

    def funcinfo(self, unwind, tries=()):
        """unwind: [(toState, action)], tries: [(lo, hi, chi, [(adj, td, disp, handler)])]."""
        g = self.g
        punw = g.words(*[w for to, act in unwind for w in (to & 0xFFFFFFFF, act)]) if unwind else 0
        tbs = []
        for lo, hi, chi, catches in tries:
            parr = g.words(*[w for c in catches for w in (c[0], c[1], c[2] & 0xFFFFFFFF, c[3])])
            tbs.append((lo, hi, chi, len(catches), parr))
        ptry = g.words(*[w for t in tbs for w in t]) if tbs else 0
        return g.words(EH_MAGIC, len(unwind), punw, len(tbs), ptry, 0, 0, 0, 1)

    def stub(self, fi):
        """The per-function handler stub: mov eax, FuncInfo; jmp __CxxFrameHandler3."""
        g = self.g

        def stub_fn(g2):
            g2.ctx["eax"] = fi
            g2.code[g2.FH3](g2)
        return g.native(stub_fn)

    def scopetable3(self, entries):
        return self.g.words(*[w for e in entries for w in (e[0] & 0xFFFFFFFF, e[1], e[2])])

    def scopetable4(self, entries):
        return self.g.words(0xFFFFFFFE, 0, 0xFFFFFFFC, 0, *[w for e in entries for w in (e[0] & 0xFFFFFFFF, e[1], e[2])])


# ---------------------------------------------------------------------------- frame helpers
def fh3_prolog(g, stub, save_esp=True):
    """push ebp; mov ebp, esp; push -1; push stub; push fs:[0]; mov fs:[0], esp; [sub esp; mov [ebp-0x10], esp]"""
    g.push(g.ctx["ebp"])
    g.ctx["ebp"] = g.ctx["esp"]
    g.push(0xFFFFFFFF)
    g.push(stub)
    g.push(g.fs0)
    g.fs0 = g.ctx["esp"]
    rn = g.ctx["esp"]
    g.ctx["esp"] -= 0x40                     # locals
    if save_esp:
        g.w32(rn - 4, g.ctx["esp"])          # mov [ebp-0x10], esp
    return rn


def fh3_epilog(g, rn):
    g.fs0 = g.r32(rn)
    g.ctx["esp"] = g.ctx["ebp"]
    g.ctx["ebp"] = g.pop()


def seh_prolog(g, handler, scopetable):
    """push ebp; mov ebp, esp; push -1/-2; push scopetable; push handler; push fs:[0]; mov fs:[0], esp;
    sub esp, N; mov [ebp-0x18], esp"""
    g.push(g.ctx["ebp"])
    g.ctx["ebp"] = g.ctx["esp"]
    g.push(0xFFFFFFFF if handler == g.EH3 else 0xFFFFFFFE)
    g.push(scopetable if handler == g.EH3 else scopetable ^ g.cookie)
    g.push(handler)
    g.push(g.fs0)
    g.fs0 = g.ctx["esp"]
    rn = g.ctx["esp"]
    g.push(0)                                 # xpointers slot (rn - 4)
    g.ctx["esp"] -= 0x20
    g.w32(rn - 8, g.ctx["esp"])
    return rn


seh_epilog = fh3_epilog
