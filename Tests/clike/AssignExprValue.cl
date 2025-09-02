int main() {
    int arr[3];
    int x;
    arr[1] = 7;
    printf("%d\n", arr[1]);
    x = (arr[1] = 9);
    printf("%d %d\n", arr[1], x);
    return 0;
}

