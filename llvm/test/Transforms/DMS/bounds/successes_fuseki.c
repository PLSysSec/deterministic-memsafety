// RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
// (we just test that we can compile this with bounds checks and then run it and
// it exits successfully, no bounds-check violations or other crashes)
// (we do test with several different optimization levels)

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

static struct patval fuseki90[] = {{0,-1}};  // dummy
static struct patval fuseki96[] = {{684,1}};

struct fullboard_pattern fuseki9[] = {
  {fuseki90, 0, "Fuseki1", 684, 504.0},
  {fuseki96, 1, "Fuseki8", 611, 173.0},
  {NULL, 0, NULL, 0, 0.0}
};

void fullboard_matchpat(struct fullboard_pattern *pattern) {
  for (; pattern->patn; pattern++) {
    if (pattern->patlen != 1) continue;
    for (volatile int k = 0; k < pattern->patlen; k++) {
      if (pattern->patn[k].att == 0) break;
    }
  }
}

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
