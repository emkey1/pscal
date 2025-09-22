int main() {
    double start = realtimeclock();
    double later = realtimeclock();

    if (start <= 0.0) {
        printf("bad\n");
        return 1;
    }
    if (later < start) {
        printf("bad\n");
        return 1;
    }

    printf("ok\n");
    return 0;
}
