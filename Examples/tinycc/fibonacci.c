int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int main() {
    int i;
    int j;
    i = 0;
    scanf(j);
    while (i < j) {
        printf(fib(i));
        i = i + 1;
    }
    return 0;
}
