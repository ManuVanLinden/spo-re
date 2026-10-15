# runtime/core: the runtime core

TEB and TLS, guest threads over pthreads, kernel objects and waits, critical sections, and the MSVC
exception-handling runtime (`__CxxFrameHandler3`, `_CxxThrowException`, `_except_handler3/4`,
`_setjmp3`/`longjmp`, `sg_eh_resume`). The design, interfaces and test results are in
[docs/design/runtime-core.md](../../docs/design/runtime-core.md).

```
make -C runtime/core test        # fixture tests (our own tables and functions; no game code)
make -C runtime/core test-san    # ASan + UBSan
make -C runtime/core test-tsan   # ThreadSanitizer
make -C runtime/core test-hle    # link with runtime/hle and check the shim registration
```

Other packages include `include/sg_core.h` and link `build/core/libcore.a`. `sg_core_register_hle_shims()` puts
the shims into runtime/hle's import table and must run before `hle_install_imports()`. Section 3 of the design
page gives the start-up sequence and explains why runtime/hle's generated stubs are weak.
