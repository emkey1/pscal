int counter;
int mid;
int i1;
int i2;

void worker1() {
    while (i1 < 100) {
        lock(mid);
        counter = counter + 1;
        unlock(mid);
        i1 = i1 + 1;
    }
}

void worker2() {
    while (i2 < 100) {
        lock(mid);
        counter = counter + 1;
        unlock(mid);
        i2 = i2 + 1;
    }
}

int main() {
    counter = 0;
    i1 = 0;
    i2 = 0;
    mid = mutex();
    int t1 = spawn worker1();
    int t2 = spawn worker2();
    join t1;
    join t2;
    printf("%d\n", counter);
    return 0;
}
