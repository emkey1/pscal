int fib(int n) {
    if (n < 2) {
        return n;
    }

    int a = 0;
    int b = 1;
    int temp;

    // Loop from 2 up to n
    for (int i = 2; i <= n; i++) {
        // Calculate the next number in the sequence
        temp = a + b;
        // Update the previous two numbers
        a = b;
        b = temp;
    }

    return b;
}

int main() {
    int i;
    int j;
    i = 0;
    clrscr();
    gotoxy(1,1);
    printf("Please enter an integer value for fibonacci: ");
    scanf(j);
    while (i < j) {
        printf(fib(i),"\n");
        i = i + 1;
    }
    return 0;
}
