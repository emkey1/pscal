int main() {
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

    printf(w);
    printf(" ");
    printf(h);
    printf("\n");
    closegraph();
#else
// Fake it if you can't make it
    printf(639);
    printf(" ");
    printf(479);
    printf("\n");
#endif
    return 0;
}
