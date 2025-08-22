int main() {
    int arr[2];
    arr[0] = 1;
    arr[1] = 2;
    int* s;
    s = &arr;
    int i;
    int j;
    i = 0;
    j = 1;
    if ((*s)[i] > (*s)[j]) {
        printf("gt\n");
    } else {
        printf("le\n");
    }
    return 0;
}
