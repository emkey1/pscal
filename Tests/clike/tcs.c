#include <stdio.h>
int main(){
    int i=2;
    switch(i){
    case 1: printf("one\n"); break;
    case 2: printf("two\n"); break;
    default: printf("other\n"); break;
    }
    char ch='b';
    switch(ch){
    case 'a': printf("A\n"); break;
    case 'b': printf("B\n"); break;
    default: printf("Z\n"); break;
    }
    return 0;
}
