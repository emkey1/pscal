int main() {
    int w;
    int h;
    initgraph(640, 480, "clike graphics test");
    w = getmaxx();
    h = getmaxy();
    printf(w);
    printf(" ");
    printf(h);
    printf("\n");
    closegraph();
    return 0;
}
