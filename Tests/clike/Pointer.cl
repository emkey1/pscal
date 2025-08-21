int main() {
    int x = 42;
    int* p;
    p = &x;
    printf(*p);
    printf("\n");
    *p = 7;
    printf(x);
    printf("\n");
    return 0;
}
