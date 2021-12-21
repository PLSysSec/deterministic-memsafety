// RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
// (we just test that we can compile this with bounds checks and then run it and
// it exits successfully, no bounds-check violations or other crashes)
// (we do test with several different optimization levels)

#include <ctype.h>
#include <stdio.h>

__attribute__((noinline))
int fprintf_to_stderr();
__attribute__((noinline))
int use_isdigit();

int main (int argc, char* argv[]) {
	fprintf_to_stderr();
	use_isdigit(argc, argv);
	return 0;
}

__attribute__((noinline))
int fprintf_to_stderr() {
	return fprintf(stderr, "hi\n");
}

__attribute__((noinline))
int use_isdigit(int argc, char* argv[]) {
  char* arg0 = argv[0];
  int i = 0;
  while (arg0[i] != '\0') {
    if (isdigit(arg0[i])) {
		  printf("Found a digit\n");
    }
    i++;
  }
  return 0;
}
