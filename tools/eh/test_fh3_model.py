"""Fixture tests for the EH reference model (tools/eh/fh3_model.py).

Every table and every "translated function" here is written by us; no game bytes are involved.
Usage:   python3 -P tools/eh/test_fh3_model.py
When:    after changing fh3_model.py (and when changing runtime/core/src/core_eh.c, which it specifies).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fh3_model import (Guest, Tables, Unhandled, fh3_prolog, fh3_epilog, seh_prolog, seh_epilog,  # noqa: E402
                       HT_IsReference, HT_IsConst, END_OF_CHAIN, STACK_TOP,
                       EXCEPTION_EXECUTE_HANDLER, EXCEPTION_CONTINUE_SEARCH)


class World:
    """A guest with one exception class (dtor + copy ctor) and logging helpers."""

    def __init__(self):
        g = self.g = Guest()
        self.T = Tables(g)
        self.td = self.T.typedesc(".?AVcError@Fixture@@")
        self.td_other = self.T.typedesc(".?AVcOther@Fixture@@")
        self.dtor = g.native(self._dtor)
        self.copy = g.native(self._copy)
        self.ti = self.T.throwinfo([self.td], dtor=self.dtor, size=8, copy=self.copy)
        self.ti_other = self.T.throwinfo([self.td_other], dtor=self.dtor, size=8, copy=self.copy)

    def _dtor(self, g):
        g.log.append(("dtor", g.ctx["ecx"]))
        g.ret()

    def _copy(self, g):
        dest, src = g.ctx["ecx"], g.r32(g.ctx["esp"] + 4)
        g.mem[dest:dest + 8] = g.mem[src:src + 8]
        g.log.append(("copy", dest, src))
        g.ret()

    def logger(self, tag, ret=None):
        """A funclet that logs and returns `ret` in eax."""
        def fn(g):
            g.log.append((tag, g.ctx["ebp"]))
            if ret is not None:
                g.ctx["eax"] = ret
            g.ret()
        return self.g.native(fn)

    def thrower(self, ti=None, value=0x1234):
        """A function that builds an exception object in its own frame and throws it."""
        def fn(g):
            g.ctx["esp"] -= 8
            obj = g.ctx["esp"]
            g.w32(obj, value)
            g.log.append(("throw", obj))
            g.cxx_throw(obj, ti or self.ti)
        return self.g.native(fn)

    def leaf_frame(self, callee, actions):
        """An FH3 frame with no try blocks: one state per action, run innermost first."""
        g = self.g
        unwind = [(i - 1, a) for i, a in enumerate(actions)]
        stub = self.T.stub(self.T.funcinfo(unwind))

        def fn(g):
            rn = fh3_prolog(g, stub)
            for i in range(len(actions)):
                g.w32(rn + 8, i)
            g.call(callee)
            g.w32(rn + 8, 0xFFFFFFFF)
            fh3_epilog(g, rn)
            g.ret()
        return g.native(fn)

    def catcher(self, callee, catch_ret_handler, unwind_actions=(0, 0), adj=HT_IsReference | HT_IsConst,
                td="mine", disp=-0x20, try_when=None, inner_local=None):
        """A function with one try block (state 0) around `callee`, catch state 1.
        Returns (address, continuation address)."""
        g = self.g
        addr_box = {}
        cont = g.native(lambda g2: None)       # an address inside this function, used only as a label
        handler = catch_ret_handler(cont)
        tdv = self.td if td == "mine" else td
        if inner_local is None:
            fi = self.T.funcinfo([(-1, unwind_actions[0]), (-1, unwind_actions[1])],
                                 [(0, 0, 1, [(adj, tdv, disp, handler)])])
        else:   # state 1 = an object constructed inside the try block
            fi = self.T.funcinfo([(-1, unwind_actions[0]), (0, inner_local), (-1, unwind_actions[1])],
                                 [(0, 1, 2, [(adj, tdv, disp, handler)])])
        stub = self.T.stub(fi)

        def fn(g):
            me = addr_box["a"]

            def body():
                rn = fh3_prolog(g, stub)
                g.w32(rn - 0x20 + 12, 0)
                if try_when is None or try_when(g):
                    g.w32(rn + 8, 0)
                    if inner_local is not None:
                        g.w32(rn + 8, 1)
                g.call(callee)
                g.w32(rn + 8, 0xFFFFFFFF)
                fh3_epilog(g, rn)
                g.ret()
                return "normal"

            def resumed():
                rn = g.ctx["ebp"] - 12
                g.log.append(("continued", me, g.ctx["ebp"], g.ctx["esp"] == g.r32(rn - 4)))
                g.w32(rn + 8, 0xFFFFFFFF)
                fh3_epilog(g, rn)
                g.ret()
                return "caught"
            return g.run_landing(me, body, {cont: resumed})
        a = g.native(fn)
        addr_box["a"] = a
        g.owner_of[cont] = a
        return a, cont

    def tags(self):
        return [e[0] for e in self.g.log]


def simple_catch(w, tag="catch"):
    def mk(cont):
        def fn(g):
            g.log.append((tag, g.r32(g.ctx["ebp"] - 0x20)))
            g.ctx["eax"] = cont
            g.ret()
        return w.g.native(fn)
    return mk


class FH3Tests(unittest.TestCase):
    def test_catch_in_same_function_runs_unwind_then_catch_then_continuation(self):
        w = World()
        g = w.g
        unw = w.logger("unwind-local")
        th = w.thrower()
        f, cont = w.catcher(th, simple_catch(w), unwind_actions=(w.logger("not-run"), 0), inner_local=unw)
        g.call(f)
        self.assertEqual(w.tags(), ["throw", "unwind-local", "catch", "dtor", "destroyed", "continued"])
        obj = g.log[0][1]
        self.assertEqual(g.log[2][1], obj, "catch by reference sees the thrown object itself")
        self.assertTrue(g.log[-1][3], "esp restored from the frame's saved-esp slot")
        self.assertEqual(g.fs0, END_OF_CHAIN)
        self.assertEqual(g.ctx["esp"], STACK_TOP)
        self.assertEqual(g.host, [])

    def test_unwinds_intermediate_frames_innermost_first(self):
        w = World()
        g = w.g
        th = w.thrower()
        n = w.leaf_frame(th, [w.logger("N0")])
        m = w.leaf_frame(n, [w.logger("M0"), w.logger("M1")])
        f, _ = w.catcher(m, simple_catch(w))
        g.call(f)
        self.assertEqual(w.tags(), ["throw", "N0", "M1", "M0", "catch", "dtor", "destroyed", "continued"])
        self.assertEqual(g.fs0, END_OF_CHAIN)

    def test_type_mismatch_passes_to_outer_and_runs_inner_destructors(self):
        w = World()
        g = w.g
        th = w.thrower()
        inner, _ = w.catcher(th, simple_catch(w, "wrong-catch"), unwind_actions=(w.logger("inner-local"), 0),
                             td=w.td_other)
        outer, _ = w.catcher(inner, simple_catch(w))
        g.call(outer)
        self.assertNotIn("wrong-catch", w.tags())
        self.assertEqual(w.tags(), ["throw", "inner-local", "catch", "dtor", "destroyed", "continued"])

    def test_catch_all(self):
        w = World()
        g = w.g
        f, _ = w.catcher(w.thrower(w.ti_other), simple_catch(w), td=0, adj=0, disp=0)
        g.call(f)
        self.assertIn("catch", w.tags())

    def test_catch_by_value_uses_copy_constructor(self):
        w = World()
        g = w.g

        def mk(cont):
            def fn(g):
                g.log.append(("catch-value", g.r32(g.ctx["ebp"] - 0x20)))
                g.ctx["eax"] = cont
                g.ret()
            return g.native(fn)
        f, _ = w.catcher(w.thrower(value=0xBEEF), mk, adj=0)
        g.call(f)
        self.assertEqual(w.tags()[:3], ["throw", "copy", "catch-value"])
        self.assertEqual(g.log[2][1], 0xBEEF, "the catch object is a copy in the catching frame")

    def test_new_throw_from_catch_destroys_the_first_object_once(self):
        w = World()
        g = w.g
        th = w.thrower(value=1)
        th2 = w.thrower(value=2)

        def mk(cont):
            def fn(g):
                g.log.append(("catch-and-throw", 0))
                g.call(th2)
            return g.native(fn)
        inner, _ = w.catcher(th, mk, unwind_actions=(0, w.logger("catch-state-local")))
        outer, _ = w.catcher(inner, simple_catch(w, "outer-catch"))
        g.call(outer)
        first, second = g.log[0][1], [e for e in g.log if e[0] == "throw"][1][1]
        # MSVC order [inf]: the catch guard sits above the catching frame in the fs:[0] chain, so the
        # caught object is destroyed before the catching frame's catch-state locals are unwound.
        self.assertEqual(w.tags(), ["throw", "catch-and-throw", "throw", "dtor", "destroyed",
                                    "catch-state-local", "outer-catch", "dtor", "destroyed", "continued"])
        self.assertEqual([e[1] for e in g.log if e[0] == "dtor"], [first, second])
        self.assertEqual(g.host, [])

    def test_rethrow_keeps_the_object_alive_until_the_outer_catch_ends(self):
        w = World()
        g = w.g

        def mk(cont):
            def fn(g):
                g.log.append(("inner-catch", 0))
                g.cxx_throw(0, 0)
            return g.native(fn)
        inner, _ = w.catcher(w.thrower(), mk)
        outer, _ = w.catcher(inner, simple_catch(w, "outer-catch"))
        g.call(outer)
        obj = g.log[0][1]
        self.assertEqual(w.tags(), ["throw", "inner-catch", "rethrow", "outer-catch", "dtor", "destroyed",
                                    "continued"])
        self.assertEqual(g.log[3][1], obj)
        self.assertEqual(sum(1 for e in g.log if e[0] == "dtor"), 1)

    def test_unhandled_runs_no_destructors(self):
        w = World()
        g = w.g
        m = w.leaf_frame(w.thrower(), [w.logger("M0")])
        with self.assertRaises(Unhandled):
            g.call(m)
        self.assertEqual(w.tags(), ["throw", "unhandled"])

    def test_recursive_instances_resume_in_the_right_frame(self):
        w = World()
        g = w.g
        depth = {"n": 0}
        th = w.thrower()
        box = {}

        def rec(g):
            depth["n"] += 1
            if depth["n"] == 3:
                g.call(th)
            g.call(box["f"])

        r = g.native(rec)
        f, _ = w.catcher(r, simple_catch(w), try_when=lambda g: depth["n"] == 0)
        box["f"] = f
        g.call(f)
        cont_events = [e for e in g.log if e[0] == "continued"]
        self.assertEqual(len(cont_events), 1)
        self.assertEqual(g.ctx["esp"], STACK_TOP, "the outermost instance resumed and returned")


class SEHTests(unittest.TestCase):
    def seh_func(self, w, handler_kind, filt_ret, inner):
        g = w.g
        filt = w.logger("filter", ret=filt_ret)
        body = g.native(lambda g2: None)
        st = (w.T.scopetable3 if handler_kind == "3" else w.T.scopetable4)([(-1 if handler_kind == "3" else -2,
                                                                              filt, body)])
        handler = g.EH3 if handler_kind == "3" else g.EH4
        box = {}

        def fn(g):
            def main():
                rn = seh_prolog(g, handler, st)
                g.w32(rn + 12, 0)
                g.call(inner)
                g.w32(rn + 12, 0xFFFFFFFF)
                seh_epilog(g, rn)
                g.ret()
                return "normal"

            def except_body():
                rn = g.ctx["ebp"] - 0x10
                g.log.append(("except-body", s(g.r32(rn + 12)), g.ctx["esp"] == g.r32(rn - 8)))
                seh_epilog(g, rn)
                g.ret()
                return "except"
            return g.run_landing(box["a"], main, {body: except_body})
        a = g.native(fn)
        box["a"] = a
        g.owner_of[body] = a
        return a

    def test_except_handler3_filter_executes_handler(self):
        w = World()
        g = w.g

        def raiser(g):
            g.log.append(("raise", 0))
            g.raise_exception(0xC0000005)
        f = self.seh_func(w, "3", EXCEPTION_EXECUTE_HANDLER, g.native(raiser))
        g.call(f)
        self.assertEqual(w.tags(), ["raise", "filter", "except-body"])
        self.assertEqual(g.log[-1][1], -1, "trylevel reset to the enclosing level")
        self.assertTrue(g.log[-1][2], "esp restored from [ebp-0x18]")
        self.assertEqual(g.fs0, END_OF_CHAIN)

    def test_cxx_throw_through_seh_frames(self):
        """A C++ exception passes an EH3 frame whose filter declines and an EH4 __finally frame."""
        w = World()
        g = w.g
        fin = w.logger("finally")
        st4 = w.T.scopetable4([(-2, 0, fin)])

        def fin_frame(g):
            rn = seh_prolog(g, g.EH4, st4)
            g.w32(rn + 12, 0)
            g.call(w.thrower())
        ff = g.native(fin_frame)
        declining = self.seh_func(w, "3", EXCEPTION_CONTINUE_SEARCH, ff)
        f, _ = w.catcher(declining, simple_catch(w))
        g.call(f)
        self.assertEqual(w.tags(), ["throw", "filter", "finally", "catch", "dtor", "destroyed", "continued"])

    def test_setjmp_longjmp_runs_intermediate_destructors(self):
        w = World()
        g = w.g
        buf = g.alloc(64)
        box = {}
        resume = g.native(lambda g2: None)

        def jumper(g):
            g.log.append(("longjmp", 0))
            g.longjmp(buf, 0)
        m = w.leaf_frame(g.native(jumper), [w.logger("M0")])

        def s_fn(g):
            def main():
                g.push(g.ctx["ebp"])
                g.ctx["ebp"] = g.ctx["esp"]
                g.ctx["esp"] -= 0x10
                g.ctx["ebx"] = 0x77
                if g.setjmp3(buf, resume) == 0:
                    g.ctx["ebx"] = 0
                    g.call(m)
                return "fell through"

            def after():
                g.log.append(("setjmp-returned", g.ctx["eax"], g.ctx["ebx"]))
                g.ctx["esp"] = g.ctx["ebp"]
                g.ctx["ebp"] = g.pop()
                g.ret()
                return "jumped"
            return g.run_landing(box["a"], main, {resume: after})
        a = g.native(s_fn)
        box["a"] = a
        g.owner_of[resume] = a
        g.call(a)
        self.assertEqual(w.tags(), ["longjmp", "M0", "setjmp-returned"])
        self.assertEqual(g.log[-1][1:], (1, 0x77), "longjmp(buf, 0) returns 1 and restores callee-saved regs")
        self.assertEqual(g.fs0, END_OF_CHAIN)
        self.assertEqual(g.ctx["esp"], STACK_TOP)


def s(v):
    return v - (1 << 32) if v & 0x80000000 else v


if __name__ == "__main__":
    unittest.main(verbosity=2)
