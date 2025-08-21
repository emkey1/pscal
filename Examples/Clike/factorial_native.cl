int fact(int n) {
    int result;
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
    printf(fact(n),"\n");
    return 0;
}
