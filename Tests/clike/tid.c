#include <stdio.h>
int main(){
    int myVar=10;
    int myArray[3]={100,200,300};
    myVar++;
    printf("myVar after ++: %d\n", myVar);
    myVar--; myVar--;
    printf("myVar after -- twice: %d\n", myVar);
    int step=5;
    myVar = myVar + step;
    printf("myVar after + step: %d\n", myVar);
    myArray[1]++;
    printf("myArray[1] after ++: %d\n", myArray[1]);
    return 0;
}
