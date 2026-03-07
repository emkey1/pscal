void main() {
    int i = 0;
    while (i < 1000000) {
        int a = i + 1;
        int b = a - 1;
        int c = b * 2;
        int d = c / 2;
        int e = d % 10;
        i = i + 1;
    }
}
