// RUN: clang -fdms=bounds -g -O0 %s -o %t && %t %t.txt
// RUN: clang -fdms=bounds -g -O1 %s -o %t && %t %t.txt
// RUN: clang -fdms=bounds -g -O3 %s -o %t && %t %t.txt
// (we just test that we can compile this with bounds checks and then run it and
// it exits successfully, no bounds-check violations or other crashes)
// (we do test with several different optimization levels)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

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

__attribute__((noinline))
void write_file(const char* filename) {
  FILE* file = fopen(filename, "w");
  if (!file) {
    perror("error opening file for writing");
    exit(1);
  }

  fprintf(file, "3 4 389\n8974 23 1\n");
  fclose(file);
}

__attribute__((noinline))
int fscanf_from_file(const char* filename, int rows) {
  FILE* file = fopen(filename, "r");
  if (!file) {
    perror("error opening file for reading");
    exit(1);
  }

  int a, b, c;
  for (int i = 0; i < rows; i++) {
    if (fscanf(file, "%d %d %d", &a, &b, &c) != 3) {
      fprintf(stderr, "fscanf error\n");
      exit(1);
    }
  }
  fclose(file);
  return a + b + c;
}

int main(int argc, char* argv[]) {
  fprintf_to_stderr();
  use_isdigit(argc, argv);
  write_file(argv[1]);
  fscanf_from_file(argv[1], 2);
  return 0;
}
