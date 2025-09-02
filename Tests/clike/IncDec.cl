int main() {
    int i = 1;
    i++;
    printf("%d\n", i);
    --i;
    printf("%d\n", i);
    i = 0;
    printf("%d %d\n", i++, ++i);
    return 0;
}

