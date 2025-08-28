void child() {
    printf("child\n");
}

int main() {
    int tid = spawn child();
    join tid;
    printf("parent\n");
    return 0;
}
