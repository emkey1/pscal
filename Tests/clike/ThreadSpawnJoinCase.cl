void WorkerThread() {
    printf("child\n");
}

int main() {
    int tid;
    tid = spawn WorkerThread();
    join tid;
    printf("parent\n");
    return 0;
}
