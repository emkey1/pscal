int main() {
    int a = 12;
    int b = 10;
    printf("12 ^ 10 = %d\n", a ^ b);

    int t = 1;
    int f = 0;
    printf("1 ^ 0 = %d\n", t ^ f);
    printf("1 ^ 1 = %d\n", t ^ t);
    printf("0 ^ 0 = %d\n", f ^ f);
    printf("logical (1 ^ 0) = %d\n", !!(t ^ f));
    return 0;
}
