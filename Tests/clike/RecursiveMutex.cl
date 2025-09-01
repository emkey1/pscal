int mid;

void worker() {
    lock(mid);
    lock(mid);
    printf("inner\n");
    unlock(mid);
    unlock(mid);
}

int main() {
    mid = rcmutex();
    int t = spawn worker();
    join t;
    destroy(mid);
    printf("done\n");
    return 0;
}
