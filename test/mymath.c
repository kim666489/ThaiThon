// mymath.c
//
// Plain C functions that get compiled with `clang -c mymath.c -o mymath.o`
// and linked straight into the ThaiThon-generated executable (see
// lib_header.json's "mymath" entry -> "source": ["mymath.c"]).
//
// If you write this in C++ instead (mymath.cpp), wrap the functions in
// `extern "C" { ... }` so their symbol names aren't C++ name-mangled --
// the LLVM IR ThaiThon emits calls them by their plain C symbol name
// (the "symbol" field in lib_header.json).

#include <math.h>

int add_numbers(int a, int b) {
    return a + b;
}

double square_root(double x) {
    return sqrt(x);
}