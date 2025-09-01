void child1() {
    printf("child1 start\n");
    printf("child1 end\n");
}

void child2() {
    printf("child2 start\n");
    printf("child2 end\n");
}

int main() {
    int tid1;
    int tid2;
    printf("parent start\n");

    // Spawn and join first child
    tid1 = spawn child1();
    join tid1;
    printf("joined child1\n");

    // Spawn and join second child
    tid2 = spawn child2();
    join tid2;
    printf("joined child2\n");

    printf("parent end\n");
    return 0;
}
