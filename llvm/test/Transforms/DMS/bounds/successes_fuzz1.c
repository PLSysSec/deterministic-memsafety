// RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
// (we just test that we can compile this with bounds checks and then run it and
// it exits successfully, no bounds-check violations or other crashes)
// (we do test with several different optimization levels)

#include <stdint.h>
a() { int8_t *b = *b = a; }
main() {}
