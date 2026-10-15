/* mt_mac.m -- the game window's CAMetalLayer for the Metal back end. Manual retain/release. */
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#include <dlfcn.h>
#include <string.h>

#include "d3d9metal.h"

/* Weak default: runtime/platform/guest's sgp_metal_layer(hwnd) (looked up with dlsym so the package links
 * without the platform layer); a child HWND without a layer presents into the main window's. */
__attribute__((weak)) void *d3d9metal_host_layer(uint32_t hwnd) {
    static void *(*metal_layer)(uint32_t);
    static uint32_t (*main_window)(void);
    static int looked;
    if (!looked) {
        *(void **)&metal_layer = dlsym(RTLD_DEFAULT, "sgp_metal_layer");
        *(void **)&main_window = dlsym(RTLD_DEFAULT, "sgp_main_window");
        looked = 1;
    }
    if (!metal_layer) return NULL;
    void *layer = metal_layer(hwnd);
    if (!layer && main_window) layer = metal_layer(main_window());
    return layer;
}

/* Diagnostics for a layer: the window that hosts it as its content layer (searched on [NSApp windows]). */
void d3d9metal_window_info(void *layerp, d3d9metal_wininfo *o) {
    memset(o, 0, sizeof *o);
    if (!layerp) return;
    CAMetalLayer *l = (CAMetalLayer *)layerp;
    CGSize ds = l.drawableSize;
    o->drawable_w = ds.width, o->drawable_h = ds.height;
    o->scale = l.contentsScale;
    o->w_pt = l.bounds.size.width, o->h_pt = l.bounds.size.height;
    if (![NSThread isMainThread] && !NSApp) return;
    for (NSWindow *w in [NSApp windows]) {
        NSView *v = w.contentView;
        if ((void *)v.layer != layerp) continue;
        o->has_window = 1;
        o->is_content_layer = 1;
        o->subviews = (int)v.subviews.count;
        o->visible = w.isVisible;
        o->key = w.isKeyWindow;
        o->scale = w.backingScaleFactor;
        o->w_pt = v.bounds.size.width, o->h_pt = v.bounds.size.height;
        break;
    }
}
