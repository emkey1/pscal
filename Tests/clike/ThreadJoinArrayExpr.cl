void child() {}

int main() {
    int tid[2];
    int i;
    tid[0] = spawn child();
    tid[1] = spawn child();
    i = 0;
    join tid[i];
    i = 1;
    join tid[i];
    printf("done\n");
    return 0;
}
