#!/usr/bin/env python3
"""Exercise the actual translated crash routine against synthetic guest objects.
Reads locally generated game C; creates no source containing game code in the repo.
The original must fault at a null target; the guarded translation must return true.
A live-target case must still run the original body and preserve its cdecl ABI.
"""
import os, pathlib, re, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parents[2]
WORK = pathlib.Path(os.environ['SPORE_WORK_DIR'])
def extract(folder):
    for p in folder.glob('*.c'):
        s = p.read_text()
        match = re.search(r'^SG_FUNC\(00d65be0\) \{.*?(?=^SG_FUNC\(|\Z)', s, re.M|re.S)
        if match:
            return match.group()
    raise RuntimeError('Creature routine not found')
original = extract(WORK/'build/sporenative/recomp-512')
fixed = extract(WORK/'build/harness/src')
assert 'sg_creature_buddy_guard(ctx, mem)' not in original
assert fixed.count('sg_creature_buddy_guard(ctx, mem)') == 1
refs = sorted(set(re.findall(r'SG_(?:CALL|TAILCALL)\(sg_f_([0-9a-f]{8})', original)))
head = '''#include "sg_recomp.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int sg_creature_buddy_guard(sg_ctx *, uint8_t *);
const uint32_t sg_dispatch_bucket[SG_DISPATCH_BUCKETS+1] = {0};
const sg_dispatch_entry sg_dispatch_entries[] = {{0}};
sg_func *sg_thunk_fn[(SG_THUNK_LIMIT-SG_THUNK_BASE)/16];
sg_miss_hook_fn sg_miss_hook;
void sg_return_mismatch(sg_ctx *ctx, uint8_t *mem, uint32_t expected) { abort(); }
static uint8_t *memory;
static void fault(int sig, siginfo_t *info, void *u) {
    if (info->si_addr == memory) _exit(90);
    _exit(91);
}
'''
stubs = ''
for ref in refs:
    head += 'SG_DECL(%s);\n' % ref
    if ref in ('00c0c240','00b5b910','00c03400','00c12560'):
        stubs += 'SG_FUNC(%s) { ctx->eax=0; SG_RET(%d); }\n' % (ref,4 if ref=='00c12560' else 0)
    else:
        stubs += 'SG_FUNC(%s) { abort(); }\n' % ref
main = r'''
int main(int argc, char **argv) {
    int present = argc > 1 && !strcmp(argv[1], "present");
    memory = mmap(NULL, 0x200000, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
    assert(memory != MAP_FAILED);
    assert(!mprotect(memory, 0x10000, PROT_NONE));
    struct sigaction sa = {0}; sa.sa_sigaction=fault; sa.sa_flags=SA_SIGINFO;
    sigaction(SIGBUS,&sa,NULL); sigaction(SIGSEGV,&sa,NULL);
    sg_ctx ctx = {0}; ctx.esp=0x1f0000; ctx.ecx=0x11111111;
    ctx.ebx=0x22222222; ctx.esi=0x33333333; ctx.edi=0x44444444; ctx.ebp=0x55555555;
    sg_st32(memory,ctx.esp,0x1234567);
    sg_st32(memory,ctx.esp+4,0x11000);
    sg_st32(memory,ctx.esp+8,0x12000);
    sg_st32(memory,ctx.esp+12,present ? 0x15000 : 0);
    sg_st32(memory,0x12000+0xb4c,0x13000);
    sg_st32(memory,0x13000+0x600,0x14000);
    if(present) sg_st32(memory,0x11000+0x38,1);
    sg_f_00d65be0(&ctx,memory);
    assert((ctx.eax&255)==1 && ctx.eip==0x1234567 && ctx.esp==0x1f0004);
    assert(ctx.ebx==0x22222222 && ctx.esi==0x33333333 && ctx.edi==0x44444444 && ctx.ebp==0x55555555);
    assert(sg_ld32(memory,0x1f000c)==(present ? 0x15000 : 0));
    munmap(memory,0x200000);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='spore-creature-test-') as td:
    for name, body in [('original',original),('fixed',fixed)]:
        src=pathlib.Path(td)/(name+'.c');exe=src.with_suffix('')
        src.write_text(head+body+stubs+main)
        subprocess.run(['clang','-std=c11','-O2','-fno-strict-aliasing','-fwrapv',
                        '-I'+str(ROOT/'runtime/include'),'-I'+str(ROOT/'recomp/include'),
                        str(src),str(ROOT/'runtime/core/src/core_creature_fix.c'),'-lm','-o',str(exe)],check=True)
        for arg,expected in [('null',90 if name=='original' else 0),('present',0)]:
            result=subprocess.run([str(exe),arg],capture_output=True,text=True)
            assert result.returncode==expected,(name,arg,result.returncode,result.stderr)
            print('%s / %s target: %s' % (name,arg,'expected null fault reproduced' if expected==90 else 'passed; cdecl stack and saved registers preserved'))
print('Creature Stage regression checks passed.')
