// RUN: clang -fdms=bounds -g -O0 -c %s -o %t.o
// RUN: clang -fdms=bounds -g -O1 -c %s -o %t.o
// RUN: clang -fdms=bounds -g -O3 -c %s -o %t.o
// (we just test that we can compile this with bounds checks, at several
// optimization levels. it won't actually run because it refers to externs,
// so it would need to link with other files)

// this file simplified from fuseki.c in 445.gobmk

#include <stdlib.h>

struct patval {
  int offset;
  int att;
};

struct fullboard_pattern {
  struct patval *patn;  // array
  int patlen;           // length of patn array
  const char *name;     // may be null
  int move_offset;
  float value;
};

extern struct fullboard_pattern fuseki9[];

void fullboard_matchpat(struct fullboard_pattern *pattern);

int main(int argc, char* argv[]) {
  struct fullboard_pattern *database;
  if (argc <= 1) {
    database = fuseki9;
  } else {
    database = NULL;
  }
  fullboard_matchpat(database);
  return 0;
}
