int fact(long n) {
    long result;
    result = 1;
    while (n > 1) {
        result = result * n;
        n = n - 1;
    }
    return result;
}

int main() {
    int n;
    clrscr();
    gotoxy(1,1);
    printf("Please enter an integer value to calculate a factorial: ");
    scanf(n);
    if (n < 0 || n > 52) {
        printf("Input must be between 0 and 20\n");
        return 0;
    }
    printf("%lld\n", fact(n));
    return 0;
}
