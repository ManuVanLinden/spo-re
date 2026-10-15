// mslrun: compile an MSL file and dispatch one compute kernel over N threads (test harness for our own
// fixtures only). Buffers: in:<index>:<file>  out:<index>:<file>:<bytes>
// Build: clang -fobjc-arc -framework Metal -framework Foundation mslrun.m -o mslrun
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc < 4) { fprintf(stderr, "usage: mslrun src.metal kernel N buffers...\n"); return 2; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        NSError *err = nil;
        NSString *src = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:&err];
        MTLCompileOptions *opt = [MTLCompileOptions new];
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe;
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:opt error:&err];
        if (!lib) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLFunction> fn = [lib newFunctionWithName:@(argv[2])];
        id<MTLComputePipelineState> ps = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (!ps) { fprintf(stderr, "%s\n", err.localizedDescription.UTF8String); return 1; }
        NSUInteger n = (NSUInteger)atol(argv[3]);
        id<MTLCommandQueue> q = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:ps];
        NSMutableArray *outs = [NSMutableArray array];
        for (int i = 4; i < argc; i++) {
            NSArray<NSString *> *f = [@(argv[i]) componentsSeparatedByString:@":"];
            NSUInteger idx = (NSUInteger)f[1].integerValue;
            id<MTLBuffer> b;
            if ([f[0] isEqualToString:@"in"]) {
                NSData *d = [NSData dataWithContentsOfFile:f[2]];
                b = [dev newBufferWithBytes:d.bytes length:MAX(d.length, 16) options:MTLResourceStorageModeShared];
            } else {
                b = [dev newBufferWithLength:(NSUInteger)f[3].integerValue options:MTLResourceStorageModeShared];
                [outs addObject:@[b, f[2]]];
            }
            [enc setBuffer:b offset:0 atIndex:idx];
        }
        [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(MIN(n, 64), 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        for (NSArray *o in outs) {
            id<MTLBuffer> b = o[0];
            [[NSData dataWithBytes:b.contents length:b.length] writeToFile:o[1] atomically:NO];
        }
        return 0;
    }
}
