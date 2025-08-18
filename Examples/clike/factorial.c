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
    scanf(n);
    printf(fact(n));
    return 0;
}
