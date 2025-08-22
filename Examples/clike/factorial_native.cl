long fact(long n) {
    long result;
    result = 1;
    while (n > 1) {
        result = result * n;
        n = n - 1;
    }
    return result;
}

int main() {
    long n;
    clrscr();
    gotoxy(1,1);
    printf("Please enter an integer value to calculate a factorial: ");
    scanf(n);
    if (n < 0 || n > 20) {
        printf("64 bit ints don't support factorial > 20\n");
        return 1;
    }
    printf("%d: %lld\n", n, fact(n));
    return 0;
}
