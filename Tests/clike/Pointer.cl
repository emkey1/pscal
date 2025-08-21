int main() {
    int x = 42;
    int* p;
    p = &x;
    printf("%d\n", *p);
    *p = 7;
    printf("%d\n", x);
    return 0;
}
