/* icon.m -- draws the Spore Native app icon with CoreGraphics. Our own artwork: a rounded square with a
 * deep-sea gradient and a tapering golden spiral of "cells", no EA art. Writes the ten PNGs of an .iconset;
 * make_bundle.py turns them into AppIcon.icns with iconutil.
 *
 *   clang -fobjc-arc -framework CoreGraphics -framework ImageIO -framework CoreFoundation -framework
 *         UniformTypeIdentifiers icon.m -o icon && ./icon OUT.iconset */
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>

static CGColorRef rgba(CGColorSpaceRef cs, double r, double g, double b, double a) {
    CGFloat c[4] = {r, g, b, a};
    return CGColorCreate(cs, c);
}

static void draw(CGContextRef cx, double S) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    /* macOS icon grid: an 824/1024 body with a ~185/1024 corner radius, centred */
    double inset = S * 100.0 / 1024.0, side = S - 2 * inset, rad = side * 0.225;
    CGRect body = CGRectMake(inset, inset, side, side);
    CGPathRef shape = CGPathCreateWithRoundedRect(body, rad, rad, NULL);

    /* drop shadow under the body */
    CGContextSaveGState(cx);
    CGColorRef sh = rgba(cs, 0, 0, 0, 0.35);
    CGContextSetShadowWithColor(cx, CGSizeMake(0, -S * 0.01), S * 0.02, sh);
    CGContextAddPath(cx, shape);
    CGColorRef base = rgba(cs, 0.05, 0.20, 0.30, 1);
    CGContextSetFillColorWithColor(cx, base);
    CGContextFillPath(cx);
    CGContextRestoreGState(cx);

    /* body: radial gradient, teal centre to deep blue edge */
    CGContextSaveGState(cx);
    CGContextAddPath(cx, shape);
    CGContextClip(cx);
    CGFloat comps[] = {0.18, 0.62, 0.62, 1, 0.07, 0.30, 0.42, 1, 0.03, 0.10, 0.22, 1};
    CGFloat locs[] = {0, 0.55, 1};
    CGGradientRef gr = CGGradientCreateWithColorComponents(cs, comps, locs, 3);
    CGPoint c = CGPointMake(S * 0.5, S * 0.52);
    CGContextDrawRadialGradient(cx, gr, c, 0, c, side * 0.75, kCGGradientDrawsAfterEndLocation);

    /* faint concentric rings (a petri dish) */
    CGColorRef ring = rgba(cs, 1, 1, 1, 0.06);
    CGContextSetStrokeColorWithColor(cx, ring);
    for (int k = 1; k <= 3; k++) {
        double r = side * 0.16 * k;
        CGContextSetLineWidth(cx, S * 0.006);
        CGContextStrokeEllipseInRect(cx, CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r));
    }

    /* the spiral: a logarithmic spiral of dots growing outwards, each with a soft glow */
    const double turns = 2.6, b = 0.19; /* r = a * e^(b * theta) */
    const double rmax = side * 0.40, tmax = turns * 2 * M_PI, a = rmax / exp(b * tmax);
    int n = 46;
    for (int i = 0; i < n; i++) {
        double t = tmax * pow((double)(i + 1) / n, 0.8);
        double r = a * exp(b * t);
        double x = c.x + r * cos(t + 0.6), y = c.y + r * sin(t + 0.6);
        double dot = S * (0.008 + 0.050 * pow((double)(i + 1) / n, 1.6));
        double f = (double)i / (n - 1);
        CGColorRef glow = rgba(cs, 1.0, 0.85, 0.35, 0.55);
        CGColorRef fill = rgba(cs, 1.0, 0.78 + 0.17 * (1 - f), 0.22 + 0.5 * (1 - f), 1);
        CGContextSaveGState(cx);
        CGContextSetShadowWithColor(cx, CGSizeZero, dot * 0.9, glow);
        CGContextSetFillColorWithColor(cx, fill);
        CGContextFillEllipseInRect(cx, CGRectMake(x - dot, y - dot, 2 * dot, 2 * dot));
        CGContextRestoreGState(cx);
        /* a nucleus in the larger cells */
        if (dot > S * 0.02) {
            CGColorRef nu = rgba(cs, 0.95, 0.45, 0.15, 0.85);
            double nr = dot * 0.38;
            CGContextSetFillColorWithColor(cx, nu);
            CGContextFillEllipseInRect(cx, CGRectMake(x - nr + dot * 0.15, y - nr + dot * 0.15, 2 * nr, 2 * nr));
            CGColorRelease(nu);
        }
        CGColorRelease(glow);
        CGColorRelease(fill);
    }
    /* the seed at the centre */
    CGColorRef seed = rgba(cs, 1, 1, 0.9, 1);
    double sr = S * 0.012;
    CGContextSetFillColorWithColor(cx, seed);
    CGContextFillEllipseInRect(cx, CGRectMake(c.x - sr, c.y - sr, 2 * sr, 2 * sr));

    /* top highlight */
    CGFloat hc[] = {1, 1, 1, 0.16, 1, 1, 1, 0};
    CGFloat hl[] = {0, 1};
    CGGradientRef hg = CGGradientCreateWithColorComponents(cs, hc, hl, 2);
    CGContextDrawLinearGradient(cx, hg, CGPointMake(0, inset + side), CGPointMake(0, inset + side * 0.55), 0);
    CGContextRestoreGState(cx);

    CGGradientRelease(hg);
    CGGradientRelease(gr);
    CGColorRelease(seed);
    CGColorRelease(ring);
    CGColorRelease(base);
    CGColorRelease(sh);
    CGPathRelease(shape);
    CGColorSpaceRelease(cs);
}

static int write_png(const char *path, int px) {
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cx = CGBitmapContextCreate(NULL, px, px, 8, 0, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextClearRect(cx, CGRectMake(0, 0, px, px));
    CGContextSetShouldAntialias(cx, true);
    CGContextSetInterpolationQuality(cx, kCGInterpolationHigh);
    draw(cx, px);
    CGImageRef img = CGBitmapContextCreateImage(cx);
    CFStringRef ps = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    CFURLRef url = CFURLCreateWithFileSystemPath(NULL, ps, kCFURLPOSIXPathStyle, false);
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL);
    int ok = d != NULL;
    if (d) {
        CGImageDestinationAddImage(d, img, NULL);
        ok = CGImageDestinationFinalize(d);
        CFRelease(d);
    }
    CFRelease(url);
    CFRelease(ps);
    CGImageRelease(img);
    CGContextRelease(cx);
    CGColorSpaceRelease(cs);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: icon OUT.iconset\n"); return 2; }
    mkdir(argv[1], 0755);
    static const int sizes[] = {16, 32, 128, 256, 512};
    for (int i = 0; i < 5; i++)
        for (int s = 1; s <= 2; s++) {
            char p[1024];
            snprintf(p, sizeof p, "%s/icon_%dx%d%s.png", argv[1], sizes[i], sizes[i], s == 2 ? "@2x" : "");
            if (write_png(p, sizes[i] * s)) { fprintf(stderr, "icon: cannot write %s\n", p); return 1; }
        }
    return 0;
}
