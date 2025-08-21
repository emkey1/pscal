int main() {
    int values[4];
    int i;

    for (i = 0; i < 4; i = i + 1) {
        values[i] = i * 10;
    }

    int *p0 = &values[3];
    int *p1 = &values[2];
    int *p2 = &values[1];
    int *p3 = &values[0];

    printf("%d %d %d %d\n", *p0, *p1, *p2, *p3);

    *p1 = *p1 + 5;
    *p3 = 99;

    int *tmp = p0;
    p0 = p3;
    p3 = tmp;

    printf("%d %d %d %d\n", *p0, *p1, *p2, *p3);

    *p2 = *p0;

    for (i = 0; i < 4; i = i + 1) {
        printf("%d\n", values[i]);
    }

    return 0;
}
