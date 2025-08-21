int main(){
    int myVar=10;
    int myArray[3];
    myArray[0]=100; myArray[1]=200; myArray[2]=300;
    myVar = myVar + 1;
    printf("myVar after ++: ");
    printf(myVar);
    printf("\n");
    myVar = myVar - 1; myVar = myVar - 1;
    printf("myVar after -- twice: ");
    printf(myVar);
    printf("\n");
    int step=5;
    myVar = myVar + step;
    printf("myVar after + step: ");
    printf(myVar);
    printf("\n");
    myArray[1] = myArray[1] + 1;
    printf("myArray[1] after ++: ");
    printf(myArray[1]);
    printf("\n");
    return 0;
}
