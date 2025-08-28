int main() {
    float r;
    int i;
    r = 5;
    printf("%.1f\n", r);
    i = 7;
    r = i;
    printf("%.1f\n", r);
    r = r + 3;
    printf("%.1f\n", r);
    printf("%.1f\n", r - 8);
    printf("%.1f\n", r - 3);
    return 0;
}
