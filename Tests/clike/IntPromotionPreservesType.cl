int main() {
    int arr[2];
    for (int y = 0; y < 2; y++) {
        float f = y * 0.5;
        arr[y] = y;
    }
    printf("%d\n", arr[0]);
    printf("%d\n", arr[1]);
    return 0;
}
