// mslcheck: compile MSL source files with the system Metal compiler (MTLDevice newLibraryWithSource),
// which works without Xcode's offline `metal` tool. Reports per-file success/errors.
// Build: clang -fobjc-arc -framework Metal -framework Foundation mslcheck.m -o mslcheck
// Usage: mslcheck [-q] file.metal...      exit status 1 if any file fails
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#pragma clang diagnostic ignored "-Wdeprecated-declarations"  // fastMathEnabled on macOS < 15

int main(int argc, const char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device\n"); return 2; }
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (@available(macOS 15.0, *)) {
            opt.mathMode = MTLMathModeSafe;          // no fast-math: keep IEEE inf/NaN behaviour
        } else {
            opt.fastMathEnabled = NO;
        }
        int quiet = 0, bad = 0, n = 0;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-q")) { quiet = 1; continue; }
            NSString *path = [NSString stringWithUTF8String:argv[i]];
            NSError *err = nil;
            NSString *src = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&err];
            if (!src) { printf("FAIL %s: unreadable\n", argv[i]); bad++; continue; }
            n++;
            id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&err];
            if (!lib) {
                bad++;
                printf("FAIL %s\n%s\n", argv[i], err.localizedDescription.UTF8String);
                continue;
            }
            // Specialise every function (function constants left undefined use their defaults).
            for (NSString *name in lib.functionNames) {
                MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
                uint32_t f = 7; // D3DCMP_GREATEREQUAL, exercises the alpha-test path
                [cv setConstantValue:&f type:MTLDataTypeUInt atIndex:0];
                NSError *e2 = nil;
                id<MTLFunction> fn = [lib newFunctionWithName:name constantValues:cv error:&e2];
                if (!fn) { bad++; printf("FAIL %s:%s %s\n", argv[i], name.UTF8String, e2.localizedDescription.UTF8String); }
            }
            if (!quiet) printf("ok   %s (%lu functions)\n", argv[i], (unsigned long)lib.functionNames.count);
        }
        printf("%d/%d compiled\n", n - bad, n);
        return bad ? 1 : 0;
    }
}
