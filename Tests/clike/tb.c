int main() {
    int i;
    printf("Testing FOR loop break...\n");
    for (i = 1; i <= 10; i = i + 1) {
        printf(i);
        printf("\n");
        if (i == 5) {
            printf("Breaking FOR loop at i = 5\n");
            break;
        }
    }
    printf("After FOR loop.\n");
    printf("\n");

    printf("Testing WHILE loop break...\n");
    i = 0;
    while (i < 10) {
        i = i + 1;
        printf(i);
        printf("\n");
        if (i == 6) {
            printf("Breaking WHILE loop at i = 6\n");
            break;
        }
    }
    printf("After WHILE loop.\n");
    printf("\n");

    printf("Testing DO-WHILE loop break...\n");
    i = 0;
    do {
        i = i + 1;
        printf(i);
        printf("\n");
        if (i == 7) {
            printf("Breaking DO-WHILE loop at i = 7\n");
            break;
        }
    } while (i < 10);
    printf("After DO-WHILE loop.\n");
    return 0;
}
