long fib(long n) {
    if (n < 2) {
        return n;
    }

    long a = 0;
    long b = 1;
    long temp;

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
    long i;
    long j;
    i = 0;
    clrscr();
    gotoxy(1,1);
    printf("Please enter an integer value for fibonacci: ");
    scanf(j);
    if(j > 92) {
       printf("Values > 92 not supported\n");
       exit();
    }
    while (i <= j) {
        printf("%d:%lld\n", i, fib(i));
        i = i + 1;
    }
    return 0;
}
