#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/Module.h"

namespace llvm {

/// Holds the IR global variables containing dynamic counts
struct DynamicCounts {
	Constant* clean;
	Constant* blemished16;
	Constant* blemished32;
	Constant* blemished64;
	Constant* blemishedconst;
	Constant* dirty;
	Constant* unknown;

  explicit DynamicCounts(Module& mod, StringRef thingToCount);
};

/// This struct holds the IR global variables representing the DYNAMIC results
/// of the analysis
class DynamicResults final {
public:
	// How many loads have a clean/dirty pointer as address
	DynamicCounts load_addrs;
	// How many stores have a clean/dirty pointer as address (we don't count the
	// data being stored, even if it's a pointer)
	DynamicCounts store_addrs;
	// How many times are we storing a clean/dirty pointer to memory (this
	// doesn't care whether the address of the store is clean or dirty)
	DynamicCounts store_vals;
	// How many times are we passing a clean/dirty pointer to a function
	DynamicCounts passed_ptrs;
	// How many times are we returning a clean/dirty pointer from a function
	DynamicCounts returned_ptrs;
	// What kinds of pointers are we doing (non-zero, but constant) pointer
	// arithmetic on? This doesn't count accessing struct fields
	DynamicCounts pointer_arith_const;
	// How many times did we produce a pointer via a 'inttoptr' instruction
	Constant* inttoptrs;

	explicit DynamicResults(Module& mod);

  /// Where to print results dynamically (at runtime)
  typedef enum PrintType {
    /// Print to stdout
    STDOUT,
    /// Print to a file in ./dms_dynamic_counts
    TOFILE,
  } PrintType;

  void addPrint(PrintType print_type);

private:
  Module& mod;
};

}
