int main() {
    printf("%d %d %d %d\n", 1 && 0, 1 && 1, 0 || 0, 0 || 1);
    printf("%d %d\n", (1 || 0) && 0, 1 || (0 && 0));
    return 0;
}

