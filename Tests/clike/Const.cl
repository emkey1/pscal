const int N = 3 + 2;

int main() {
    int arr[N];
    printf("%d %d\n", N, sizeof(arr));
    switch (N) {
        case 5: printf("C\n"); break;
        default: printf("X\n");
    }
    return 0;
}

