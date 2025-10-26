void child() {
    printf("child\n");
}

int main() {
    int i;
    for (i = 0; i < 6; i++) {
        int tid = spawn child();
        join tid;
    }
    printf("parent\n");
    return 0;
}
