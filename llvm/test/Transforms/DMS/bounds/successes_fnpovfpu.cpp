// RUN: clang++ -fdms=bounds -g -O0 %s -o %t && %t
// RUN: clang++ -fdms=bounds -g -O1 %s -o %t && %t
// RUN: clang++ -fdms=bounds -g -O3 %s -o %t && %t
// (we just test that we can compile this with bounds checks and then run it and
// it exits successfully, no bounds-check violations or other crashes)
// (we do test with several different optimization levels)

// this file simplified from fnpovfpu.cpp in 453.povray

struct FunctionCode {
  unsigned* program;
  unsigned int program_size;
  const char *name;
};

struct FunctionEntry {
  FunctionCode fn;
  unsigned int reference_count;
};

FunctionEntry Functions[3];

void RemoveFunction(unsigned fn) {
  if (Functions[fn].reference_count > 0) {
    Functions[fn].reference_count--;
    if (Functions[fn].reference_count == 0) {
      FunctionEntry f = Functions[fn];
      for (int i = 0; i < f.fn.program_size; i++) {
        if ((f.fn.program[i] & 0xfff) == 15) {
          RemoveFunction((f.fn.program[i] >> 12) & 0xfffff);
        }
      }
    }
  }
}

unsigned someprog[] = {2, 76, 101, 33, 754};
unsigned otherprog[] = {1001, 888, 3, 13, 44, 2, 919, 2049};

int main(int argc, char* argv[]) {
  Functions[0] = FunctionEntry {
    FunctionCode {
      someprog,
      5,
      "someprog"
    },
    3,
  };
  Functions[1] = FunctionEntry {
    FunctionCode {
      otherprog,
      1,
      "otherprog"
    },
    1,
  };
  Functions[2] = FunctionEntry {
    FunctionCode {
      otherprog,
      8,
      "anotherprog"
    },
    1,
  };
	RemoveFunction(1);
  return 0;
}
