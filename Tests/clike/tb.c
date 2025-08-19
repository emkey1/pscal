int main() {
    int i;
    printf("Testing FOR loop break...");
    for (i = 1; i <= 10; i = i + 1) {
        printf(i);
        if (i == 5) {
            printf("Breaking FOR loop at i = 5");
            break;
        }
    }
    printf("After FOR loop.");
    printf("");

    printf("Testing WHILE loop break...");
    i = 0;
    while (i < 10) {
        i = i + 1;
        printf(i);
        if (i == 6) {
            printf("Breaking WHILE loop at i = 6");
            break;
        }
    }
    printf("After WHILE loop.");
    printf("");

    printf("Testing DO-WHILE loop break...");
    i = 0;
    do {
        i = i + 1;
        printf(i);
        if (i == 7) {
            printf("Breaking DO-WHILE loop at i = 7");
            break;
        }
    } while (i < 10);
    printf("After DO-WHILE loop.");
    return 0;
}
