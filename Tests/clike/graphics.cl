// Request a keypress before exiting to allow interactive inspection of the
// rendered graphics.
#include <stdio.h>

int main() {
    str dummy;
#ifdef SDL_ENABLED
    int w;
    int h;
    initgraph(640, 480, "clike graphics test");
    w = getmaxx();
    h = getmaxy();

    cleardevice();
    setcolor(15);
    putpixel(5, 5);
    drawline(0, 0, w, h);
    drawrect(w / 4, h / 4, w * 3 / 4, h * 3 / 4);
    updatescreen();
    graphloop(10);

    printf("%d %d\n", w, h);
    closegraph();
#else
// Fake it if you can't make it
    printf("639 479\n");
#endif
    printf("Press Enter to exit...\n");
    scanf(dummy);
    return 0;
}
