int mathCategory() {
#ifdef extended math
    return 1;
#else
    return 0;
#endif
}

int mathCategoryElseif() {
#ifdef extended missing_category
    return 1;
#elseif extended math
    return 2;
#else
    return 3;
#endif
}

int factorialAvailable() {
#ifdef extended math Factorial
    return 1;
#else
    return 0;
#endif
}

int imaginaryFunction() {
#ifdef extended math Imaginary
    return 1;
#else
    return 0;
#endif
}

int main() {
    printf("%d %d %d %d\n", mathCategory(), mathCategoryElseif(),
           factorialAvailable(), imaginaryFunction());
    return 0;
}
