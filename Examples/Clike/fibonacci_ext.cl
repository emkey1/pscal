int main() {
    int i;
    int j;
    i = 0;
    clrscr();
    gotoxy(1,1);
    printf("Please enter an integer value for fibonacci: ");
    scanf(j);
    while (i <= j) {
        printf(fibonacci(i),"\n");
        i = i + 1;
    }
    return 0;
}
