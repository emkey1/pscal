int counter;
int mid;

void worker() {
    int i = 0;
    while (i < 100) {
        lock(mid);
        counter = counter + 1;
        unlock(mid);
        i = i + 1;
    }
}

int main() {
    counter = 0;
    mid = mutex();
    int t1 = spawn worker();
    int t2 = spawn worker();
    join t1;
    join t2;
    printf("%d\n", counter);
    return 0;
}
