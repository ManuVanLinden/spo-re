/* test_null_suite.c -- runs runtime/d3d9null/tests/test_d3d9.c (compiled with main renamed) against the
 * Metal back end: SPORE_D3D9=metal makes runtime/d3d9null's Direct3DCreate9 pick d3d9metal_create. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d3d9metal.h"
int null_suite_main(void);
void test_app_init(void);
void test_window_close(void);
int main(void) {
    int r;
    test_app_init();
    if (!getenv("SPORE_D3D9")) setenv("SPORE_D3D9", "metal", 1);
    r = null_suite_main();
    fprintf(stderr, "back end: %s\n", d3d9metal_loaded() ? "metal" : "null (Metal back end not started)");
    test_window_close();
    return r;
}
