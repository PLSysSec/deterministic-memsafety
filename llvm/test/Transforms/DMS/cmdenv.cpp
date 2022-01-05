// RUN: clang++ -fdms=bounds -g -O0 -c %s -o %t.o
// RUN: clang++ -fdms=bounds -g -O1 -c %s -o %t.o
// RUN: clang++ -fdms=bounds -g -O3 -c %s -o %t.o
// (we just test that we can compile this with bounds checks, at several
// optimization levels. it won't actually run because it refers to externs,
// so it would need to link with other files)

// this file simplified from cmdenv.cc in 471.omnetpp

#include <cstdlib>
#include <assert.h>

class App {
  public:
    virtual void displayMessage(int e);
    void simulate();
};

extern bool sigint_received;

void* selectNextModule();

void App::simulate() {
  try {
    void* mod = selectNextModule();
    assert(mod!=NULL);
    if (sigint_received)
      throw 1;
  } catch (int e) {
    displayMessage(e);
  }
}
