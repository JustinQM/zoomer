#include <stdio.h>
#include "ext-image-copy-capture.h"
#include "wlr-layer-shell-unstable-v1.h"

int main(void)
{
    printf("Hello World!\n");

    struct wl_display* display = wl_display_connect(NULL);
    printf("%p\n", display);

    return 0;
}
