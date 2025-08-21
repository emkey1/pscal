int main(){
    int myVar=10;
    int myArray[3];
    myArray[0]=100; myArray[1]=200; myArray[2]=300;
    myVar = myVar + 1;
    printf("myVar after ++: %d\n", myVar);
    myVar = myVar - 1; myVar = myVar - 1;
    printf("myVar after -- twice: %d\n", myVar);
    int step=5;
    myVar = myVar + step;
    printf("myVar after + step: %d\n", myVar);
    myArray[1] = myArray[1] + 1;
    printf("myArray[1] after ++: %d\n", myArray[1]);
    return 0;
}
