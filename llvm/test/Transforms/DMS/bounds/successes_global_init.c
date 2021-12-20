// RUN: clang -fdms=bounds -g -O0 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O1 %s -o %t && %t
// RUN: clang -fdms=bounds -g -O3 %s -o %t && %t
// (we just test that we can compile this with bounds checks and then run it and
// it exits successfully, no bounds-check violations or other crashes)
// (we do test with several different optimization levels)

#include <stdlib.h>
#include <string.h>

typedef struct {
  volatile int a;
  volatile int b;
  volatile int c;
} Config;

Config config;

volatile int* Values[] = {
  &config.a,
  &config.b,
  &config.c,
  NULL
};

volatile int** MoreValues[] = {
	&Values[2],
	&Values[1],
	&Values[3],
	&Values[0],
	NULL
};

int main(int argc, char* argv[]) {
  memset(&config, 0, sizeof (Config));
  volatile int y = *Values[0];
  volatile int z = *Values[1];
	volatile int* ptr = *MoreValues[2];
	return 0;
}
