#include "llvm/Transforms/Utils/DLIM.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Regex.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <sstream>  // ostringstream

using namespace llvm;

#define DEBUG_TYPE "DLIM"

static void describePointerList(const SmallVector<const Value*, 8>& ptrs, std::ostringstream& out, StringRef desc);
static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep);
static bool isOffsetAnInductionPattern(const GetElementPtrInst &gep, const DataLayout &DL, const LoopInfo &loopinfo, const PostDominatorTree &pdtree, /* output */ APInt* out_induction_offset, /* output */ APInt* out_initial_offset);
static bool isInductionVar(const Value* val, /* output */ APInt* out_induction_increment, /* output */ APInt* out_initial_val);
static bool isValuePlusConstant(const Value* val, /* output */ const Value** out_val, /* output */ APInt* out_const);
static bool isAllocatingCall(const CallBase &call);
static Constant* createGlobalConstStr(Module* mod, const char* global_name, const char* str);
static std::string regexSubAll(const Regex &R, const StringRef Repl, const StringRef String);

typedef enum PointerKind {
  // As of this writing, the operations producing UNKNOWN are: loading a pointer
  // from memory; returning a pointer from a call; and receiving a pointer as a
  // function parameter
  UNKNOWN = 0,
  // CLEAN means "not modified since last allocated or dereferenced"
  CLEAN,
  // BLEMISHED16 means "incremented by 16 bytes or less from a clean pointer"
  BLEMISHED16,
  // BLEMISHED32 means "incremented by 32 bytes or less from a clean pointer".
  // We'll make some effort to keep BLEMISHED16 pointers out of this bucket, but
  // if we can't determine which bucket it belongs in, it conservatively goes
  // here.
  BLEMISHED32,
  // BLEMISHED64 means "incremented by 64 bytes or less from a clean pointer".
  // We'll make some effort to keep BLEMISHED16 and BLEMISHED32 pointers out of
  // this bucket, but if we can't determine which bucket it belongs in, it
  // conservatively goes here.
  BLEMISHED64,
  // BLEMISHEDCONST means "incremented by some compile-time-constant number of
  // bytes from a clean pointer".
  // We'll make some effort to keep BLEMISHED16 / BLEMISHED32 / BLEMISHED64
  // pointers out of this bucket (leaving this bucket just for constants greater
  // than 64), but if we can't determine which bucket it belongs in, it
  // conservatively goes here.
  BLEMISHEDCONST,
  // DIRTY means "may have been incremented by a non-compile-time-constant
  // amount (or decremented by any amount) since last allocated or dereferenced"
  DIRTY,
  // NOTDEFINEDYET means that the pointer has not been defined yet at this program
  // point (at least, to our current knowledge). All pointers are (effectively)
  // initialized to NOTDEFINEDYET at the beginning of the fixpoint analysis, and
  // as we iterate we gradually refine this.
  NOTDEFINEDYET,
} PointerKind;

/// Merge two `PointerKind`s.
/// For the purposes of this function, the ordering is
/// DIRTY < UNKNOWN < BLEMISHEDCONST < BLEMISHED64 < BLEMISHED32 < BLEMISHED16 < CLEAN,
/// and the merge returns the least element.
/// NOTDEFINEDYET has the property where the merger of x and NOTDEFINEDYET is x
/// (for all x) - for instance, if we are at a join point in the CFG where the
/// pointer is x status on one incoming branch and not defined on the other,
/// the pointer can have x status going forward.
static PointerKind merge(const PointerKind a, const PointerKind b) {
  if (a == NOTDEFINEDYET) {
    return b;
  } else if (b == NOTDEFINEDYET) {
    return a;
  } else if (a == DIRTY || b == DIRTY) {
    return DIRTY;
  } else if (a == UNKNOWN || b == UNKNOWN) {
    return UNKNOWN;
  } else if (a == BLEMISHEDCONST || b == BLEMISHEDCONST) {
    return BLEMISHEDCONST;
  } else if (a == BLEMISHED64 || b == BLEMISHED64) {
    return BLEMISHED64;
  } else if (a == BLEMISHED32 || b == BLEMISHED32) {
    return BLEMISHED32;
  } else if (a == BLEMISHED16 || b == BLEMISHED16) {
    return BLEMISHED16;
  } else if (a == CLEAN && b == CLEAN) {
    return CLEAN;
  } else {
    assert(false && "Missing case in merge function");
  }
}

static PointerKind classifyGEPResult(const GetElementPtrInst &gep, const PointerKind input_kind, const DataLayout &DL, const bool trustLLVMStructTypes, const APInt* override_constant_offset, /* output */ bool* offsetIsNonzeroConstant);

/// Conceptually stores the PointerKind of all currently valid pointers at a
/// particular program point.
class PointerStatuses {
public:
  PointerStatuses(const DataLayout &DL, const bool trustLLVMStructTypes)
    : DL(DL), trustLLVMStructTypes(trustLLVMStructTypes) {}

  PointerStatuses(const PointerStatuses& other)
    : DL(other.DL), trustLLVMStructTypes(other.trustLLVMStructTypes), map(other.map) {}

  PointerStatuses operator=(const PointerStatuses& other) {
    assert(DL == other.DL);
    assert(trustLLVMStructTypes == other.trustLLVMStructTypes);
    map = other.map;
    return *this;
  }

  void mark_clean(const Value* ptr) {
    mark_as(ptr, CLEAN);
  }

  void mark_dirty(const Value* ptr) {
    mark_as(ptr, DIRTY);
  }

  void mark_blemished16(const Value* ptr) {
    mark_as(ptr, BLEMISHED16);
  }

  void mark_blemished32(const Value* ptr) {
    mark_as(ptr, BLEMISHED32);
  }

  void mark_blemished64(const Value* ptr) {
    mark_as(ptr, BLEMISHED64);
  }

  void mark_blemishedconst(const Value* ptr) {
    mark_as(ptr, BLEMISHEDCONST);
  }

  void mark_unknown(const Value* ptr) {
    mark_as(ptr, UNKNOWN);
  }

  void mark_as(const Value* ptr, PointerKind kind) {
    // don't explicitly mark anything NOTDEFINEDYET - we reserve
    // "not in the map" to mean NOTDEFINEDYET
    assert(kind != NOTDEFINEDYET);
    // insert() does nothing if the key was already in the map.
    // instead, it appears we have to use operator[], which seems to
    // work whether or not `ptr` was already in the map
    map[ptr] = kind;
  }

  PointerKind getStatus(const Value* ptr) const {
    auto it = map.find(ptr);
    if (it != map.end()) {
      // found it in the map
      return it->getSecond();
    }
    // if we get here, the pointer wasn't found in the map. Is it a constant pointer?
    if (const Constant* constant = dyn_cast<Constant>(ptr)) {
      if (constant->isNullValue()) {
        // the null pointer can be considered CLEAN
        return CLEAN;
      } else if (const ConstantExpr* expr = dyn_cast<ConstantExpr>(constant)) {
        // it's a pointer created by a compile-time constant expression
        if (expr->isGEPWithNoNotionalOverIndexing()) {
          // this seems sufficient to consider the pointer clean, based on docs
          // of this method. GEP on a constant pointer, with constant indices,
          // that LLVM thinks are all in-bounds
          return CLEAN;
        }
        switch (expr->getOpcode()) {
          case Instruction::BitCast: {
            // bitcast doesn't change the status
            return getStatus(expr->getOperand(0));
          }
          case Instruction::GetElementPtr: {
            // constant-GEP expression
            const Instruction* inst = expr->getAsInstruction();
            const GetElementPtrInst* gepinst = cast<GetElementPtrInst>(inst);
            bool dontcare;
            return classifyGEPResult(*gepinst, getStatus(gepinst->getPointerOperand()), DL, trustLLVMStructTypes, NULL, &dontcare);
          }
          default: {
            assert(false && "getting status of constant expression of unhandled opcode");
          }
        }
      } else {
        // a constant, but not null and not a constant expression.
        assert(false && "getting status of constant pointer of unhandled kind");
      }
    } else {
      // not found in map, and not a constant.
      return NOTDEFINEDYET;
    }
  }

  size_t numClean() const {
    unsigned count = 0;
    for (auto& pair : map) {
      if (pair.getSecond() == CLEAN) {
        count++;
      }
    }
    return count;
  }

  bool isEqualTo(const PointerStatuses& other) const {
    // first we check if every pair in A is also in B
    for (const auto &pair : map) {
      const auto& it = other.map.find(pair.getFirst());
      if (it == other.map.end()) {
        // key wasn't in other.map
        if (pair.getSecond() == NOTDEFINEDYET) {
          // missing from other.map, but marked NOTDEFINEDYET in this map: the maps are
          // still equivalent (missing is implicitly NOTDEFINEDYET)
          continue;
        } else {
          // missing from other.map, but defined in this map
          return false;
        }
      }
      if (it->getSecond() != pair.getSecond()) {
        // maps disagree on what this key maps to
        return false;
      }
    }
    // now we check if every pair in B is also in A
    for (const auto &pair : other.map) {
      const auto &it = map.find(pair.getFirst());
      if (it == map.end()) {
        // key wasn't in this map
        if (pair.getSecond() == NOTDEFINEDYET) {
          // missing from this map, but marked NOTDEFINEDYET in other.map: the maps are
          // still equivalent (missing is implicitly NOTDEFINEDYET)
          continue;
        } else {
          // missing from this map, but defined in other.map
          return false;
        }
      }
      // if the key is in both maps, we already checked above that the maps
      // agree on what this key maps to
    }
    return true;
  }

  std::string describe() const {
    SmallVector<const Value*, 8> clean_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> blem_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> dirty_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> unk_ptrs = SmallVector<const Value*, 8>();
    for (auto& pair : map) {
      const Value* ptr = pair.getFirst();
      if (ptr->getNameOrAsOperand().rfind("__DLIM", 0) == 0) {
        // name starts with __DLIM, skip it
        continue;
      }
      switch (pair.getSecond()) {
        case CLEAN:
          clean_ptrs.push_back(ptr);
          break;
        case BLEMISHED16:
        case BLEMISHED32:
        case BLEMISHED64:
        case BLEMISHEDCONST:
          blem_ptrs.push_back(ptr);
          break;
        case DIRTY:
          dirty_ptrs.push_back(ptr);
          break;
        case UNKNOWN:
          unk_ptrs.push_back(ptr);
          break;
        default:
          break;
      }
    }
    std::ostringstream out;
    describePointerList(clean_ptrs, out, "clean");
    out << " and ";
    describePointerList(blem_ptrs, out, "blem");
    out << " and ";
    describePointerList(dirty_ptrs, out, "dirty");
    out << " and ";
    describePointerList(unk_ptrs, out, "unk");
    return out.str();
  }

  /// Merge the two given PointerStatuses. If they disagree on any pointer,
  /// use the `PointerKind` `merge` function to combine the two results.
  /// Recall that any pointer not appearing in the `map` is considered NOTDEFINEDYET.
  static PointerStatuses merge(const PointerStatuses& a, const PointerStatuses& b) {
    assert(a.DL == b.DL);
    assert(a.trustLLVMStructTypes == b.trustLLVMStructTypes);
    PointerStatuses merged(a.DL, a.trustLLVMStructTypes);
    for (const auto& pair : a.map) {
      const Value* ptr = pair.getFirst();
      const PointerKind kind_in_a = pair.getSecond();
      const auto& it = b.map.find(ptr);
      PointerKind kind_in_b;
      if (it == b.map.end()) {
        // implicitly NOTDEFINEDYET in b
        kind_in_b = NOTDEFINEDYET;
      } else {
        kind_in_b = it->getSecond();
      }
      merged.mark_as(ptr, ::merge(kind_in_a, kind_in_b));
    }
    // at this point we've handled all the pointers which were defined in a.
    // what's left is the pointers which were defined in b and NOTDEFINEDYET in a
    for (const auto& pair : b.map) {
      const Value* ptr = pair.getFirst();
      const PointerKind kind_in_b = pair.getSecond();
      const auto& it = a.map.find(ptr);
      if (it == a.map.end()) {
        // implicitly NOTDEFINEDYET in a
        merged.mark_as(ptr, ::merge(NOTDEFINEDYET, kind_in_b));
      }
    }
    return merged;
  }

private:
  const DataLayout &DL;
  const bool trustLLVMStructTypes;
  /// Pointers not appearing in this map are considered NOTLIVE.
  /// As a corollary, hopefully all pointers which are currently live do appear
  /// in this map.
  SmallDenseMap<const Value*, PointerKind, 8> map;
};

/// This holds the per-block state for the analysis
class PerBlockState {
public:
  PerBlockState(const DataLayout &DL, const bool trustLLVMStructTypes)
    : ptrs_beg(PointerStatuses(DL, trustLLVMStructTypes)), ptrs_end(PointerStatuses(DL, trustLLVMStructTypes)) {}

  /// The status of all pointers at the _beginning_ of the block.
  PointerStatuses ptrs_beg;
  /// The status of all pointers at the _end_ of the block.
  PointerStatuses ptrs_end;
};

class DLIMAnalysis {
public:
  /// Creates and initializes the Analysis but doesn't actually run the analysis.
  ///
  /// `trustLLVMStructTypes`: if `true`, then we will assume that, e.g., if we have
  /// a CLEAN pointer to a struct, and derive a pointer to the nth element of that
  /// struct, the resulting pointer is also CLEAN.
  /// This assumption could get us in trouble if the original "pointer to a struct"
  /// was actually a pointer to some smaller object, and was casted to this pointer type.
  /// E.g., this could happen if we incorrectly cast a `void*` to a struct pointer in C.
  ///
  /// `inttoptr_kind`: the `PointerKind` to use for pointers generated by
  /// `inttoptr` instructions, i.e., by casting an integer to a pointer. This
  /// can be any `PointerKind` -- e.g., CLEAN, DIRTY, UNKNOWN, etc.
  DLIMAnalysis(Function &F, FunctionAnalysisManager &FAM, bool trustLLVMStructTypes, PointerKind inttoptr_kind)
    : F(F), FAM(FAM), DL(F.getParent()->getDataLayout()),
      trustLLVMStructTypes(trustLLVMStructTypes), inttoptr_kind(inttoptr_kind),
      RPOT(ReversePostOrderTraversal<BasicBlock *>(&F.getEntryBlock())) {
    initialize_block_states();
  }
  ~DLIMAnalysis() {}

  typedef struct StaticCounts {
    unsigned clean;
    unsigned blemished16;
    unsigned blemished32;
    unsigned blemished64;
    unsigned blemishedconst;
    unsigned dirty;
    unsigned unknown;
  } StaticCounts;

  /// This struct holds the STATIC results of the analysis
  typedef struct StaticResults {
    // How many loads have a clean/dirty pointer as address
    StaticCounts load_addrs;
    // How many stores have a clean/dirty pointer as address (we don't count the
    // data being stored, even if it's a pointer)
    StaticCounts store_addrs;
    // How many times are we storing a clean/dirty pointer to memory (this
    // doesn't care whether the address of the store is clean or dirty)
    StaticCounts store_vals;
    // How many times are we passing a clean/dirty pointer to a function
    StaticCounts passed_ptrs;
    // How many times are we returning a clean/dirty pointer from a function
    StaticCounts returned_ptrs;
    // What kinds of pointers are we doing (non-zero, but constant) pointer
    // arithmetic on? This doesn't count accessing struct fields
    StaticCounts pointer_arith_const;
    // How many times did we produce a pointer via a 'inttoptr' instruction
    unsigned inttoptrs;
  } StaticResults;

  /// Holds the IR global variables containing dynamic counts
  typedef struct DynamicCounts {
    Constant* clean;
    Constant* blemished16;
    Constant* blemished32;
    Constant* blemished64;
    Constant* blemishedconst;
    Constant* dirty;
    Constant* unknown;
  } DynamicCounts;

  /// This struct holds the IR global variables representing the DYNAMIC results
  /// of the analysis
  typedef struct DynamicResults {
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
  } DynamicResults;

  /// Runs the analysis and returns the `StaticResults`
  StaticResults run() {
    StaticResults static_results;

    bool changed = true;
    while (changed) {
      LLVM_DEBUG(dbgs() << "DLIM: starting an iteration through function " << F.getName() << "\n");
      changed = doIteration(static_results, NULL, false);
    }

    return static_results;
  }

  /// Where to print results dynamically (at runtime)
  typedef enum DynamicPrintType {
    /// Print to stdout
    STDOUT,
    /// Print to a file in ./dlim_dynamic_counts
    TOFILE,
  } DynamicPrintType;

  /// Instruments the code for dynamic counts.
  /// You _must_ run() the analysis first -- instrument() assumes that the
  /// analysis is complete.
  void instrument(DynamicPrintType print_type) {
    DynamicResults results = initializeDynamicResults();
    StaticResults _ignore;
    bool changed = doIteration(_ignore, &results, true);
    assert(!changed && "Don't run instrument() until analysis has reached fixpoint");
    addDynamicResultsPrint(results, print_type);
  }

  void reportStaticResults(StaticResults& results) {
    dbgs() << "Static counts for " << F.getName() << ":\n";
    dbgs() << "Loads with clean addr: " << results.load_addrs.clean << "\n";
    dbgs() << "Loads with blemished16 addr: " << results.load_addrs.blemished16 << "\n";
    dbgs() << "Loads with blemished32 addr: " << results.load_addrs.blemished32 << "\n";
    dbgs() << "Loads with blemished64 addr: " << results.load_addrs.blemished64 << "\n";
    dbgs() << "Loads with blemishedconst addr: " << results.load_addrs.blemishedconst << "\n";
    dbgs() << "Loads with dirty addr: " << results.load_addrs.dirty << "\n";
    dbgs() << "Loads with unknown addr: " << results.load_addrs.unknown << "\n";
    dbgs() << "Stores with clean addr: " << results.store_addrs.clean << "\n";
    dbgs() << "Stores with blemished16 addr: " << results.store_addrs.blemished16 << "\n";
    dbgs() << "Stores with blemished32 addr: " << results.store_addrs.blemished32 << "\n";
    dbgs() << "Stores with blemished64 addr: " << results.store_addrs.blemished64 << "\n";
    dbgs() << "Stores with blemishedconst addr: " << results.store_addrs.blemishedconst << "\n";
    dbgs() << "Stores with dirty addr: " << results.store_addrs.dirty << "\n";
    dbgs() << "Stores with unknown addr: " << results.store_addrs.unknown << "\n";
    dbgs() << "Storing a clean ptr to mem: " << results.store_vals.clean << "\n";
    dbgs() << "Storing a blemished16 ptr to mem: " << results.store_vals.blemished16 << "\n";
    dbgs() << "Storing a blemished32 ptr to mem: " << results.store_vals.blemished32 << "\n";
    dbgs() << "Storing a blemished64 ptr to mem: " << results.store_vals.blemished64 << "\n";
    dbgs() << "Storing a blemishedconst ptr to mem: " << results.store_vals.blemishedconst << "\n";
    dbgs() << "Storing a dirty ptr to mem: " << results.store_vals.dirty << "\n";
    dbgs() << "Storing an unknown ptr to mem: " << results.store_vals.unknown << "\n";
    dbgs() << "Passing a clean ptr to a func: " << results.passed_ptrs.clean << "\n";
    dbgs() << "Passing a blemished16 ptr to a func: " << results.passed_ptrs.blemished16 << "\n";
    dbgs() << "Passing a blemished32 ptr to a func: " << results.passed_ptrs.blemished32 << "\n";
    dbgs() << "Passing a blemished64 ptr to a func: " << results.passed_ptrs.blemished64 << "\n";
    dbgs() << "Passing a blemishedconst ptr to a func: " << results.passed_ptrs.blemishedconst << "\n";
    dbgs() << "Passing a dirty ptr to a func: " << results.passed_ptrs.dirty << "\n";
    dbgs() << "Passing an unknown ptr to a func: " << results.passed_ptrs.unknown << "\n";
    dbgs() << "Returning a clean ptr from a func: " << results.returned_ptrs.clean << "\n";
    dbgs() << "Returning a blemished16 ptr from a func: " << results.returned_ptrs.blemished16 << "\n";
    dbgs() << "Returning a blemished32 ptr from a func: " << results.returned_ptrs.blemished32 << "\n";
    dbgs() << "Returning a blemished64 ptr from a func: " << results.returned_ptrs.blemished64 << "\n";
    dbgs() << "Returning a blemishedconst ptr from a func: " << results.returned_ptrs.blemishedconst << "\n";
    dbgs() << "Returning a dirty ptr from a func: " << results.returned_ptrs.dirty << "\n";
    dbgs() << "Returning an unknown ptr from a func: " << results.returned_ptrs.unknown << "\n";
    dbgs() << "Nonzero constant pointer arithmetic on a clean ptr: " << results.pointer_arith_const.clean << "\n";
    dbgs() << "Nonzero constant pointer arithmetic on a blemished16 ptr: " << results.pointer_arith_const.blemished16 << "\n";
    dbgs() << "Nonzero constant pointer arithmetic on a blemished32 ptr: " << results.pointer_arith_const.blemished32 << "\n";
    dbgs() << "Nonzero constant pointer arithmetic on a blemished64 ptr: " << results.pointer_arith_const.blemished64 << "\n";
    dbgs() << "Nonzero constant pointer arithmetic on a blemishedconst ptr: " << results.pointer_arith_const.blemishedconst << "\n";
    dbgs() << "Nonzero constant pointer arithmetic on a dirty ptr: " << results.pointer_arith_const.dirty << "\n";
    dbgs() << "Nonzero constant pointer arithmetic on an unknown ptr: " << results.pointer_arith_const.unknown << "\n";
    dbgs() << "Producing a ptr from inttoptr: " << results.inttoptrs << "\n";
    dbgs() << "\n";
  }

private:
  Function &F;
  FunctionAnalysisManager &FAM;
  const DataLayout &DL;
  const bool trustLLVMStructTypes;
  const PointerKind inttoptr_kind;

  /// we use "reverse post order" in an attempt to process block predecessors
  /// before the blocks themselves. (Of course, this isn't possible to do
  /// perfectly, because of loops.)
  /// We also store this as a class member because constructing it is expensive,
  /// according to the docs in PostOrderIterator.h. We don't want to construct it
  /// each time it's needed in `doIteration()`.
  const ReversePostOrderTraversal<BasicBlock*> RPOT;

  DenseMap<const BasicBlock*, PerBlockState*> block_states;

  void initialize_block_states() {
    for (const BasicBlock &block : F) {
      block_states.insert(
        std::pair<const BasicBlock*, PerBlockState*>(&block, new PerBlockState(DL, trustLLVMStructTypes))
      );
    }

    // For now, if any function parameters are pointers,
    // mark them UNKNOWN in the function's entry block
    PerBlockState* entry_block_pbs = block_states[&F.getEntryBlock()];
    for (const Argument& arg : F.args()) {
      if (arg.getType()->isPointerTy()) {
        entry_block_pbs->ptrs_beg.mark_unknown(&arg);
      }
    }

    // Mark pointers to global variables as CLEAN in the function's entry block
    // (if the global variable itself is a pointer, it's still implicitly dirty)
    for (const GlobalVariable& gv : F.getParent()->globals()) {
      assert(gv.getType()->isPointerTy());
      entry_block_pbs->ptrs_beg.mark_clean(&gv);
    }
  }

  /// `instrument`: if `true`, insert instrumentation to collect dynamic counts.
  /// Do this only after the analysis has reached a fixpoint.
  ///
  /// Returns `true` if any change was made to internal state (not counting the
  /// results objects of course)
  bool doIteration(StaticResults &static_results, DynamicResults* dynamic_results, bool instrument) {
    // Reset the static results - we'll collect them new
    static_results = { 0 };
    bool changed = false;

    for (BasicBlock* block : RPOT) {
      changed |= analyze_block(*block, static_results, dynamic_results, instrument);
    }

    return changed;
  }

  bool analyze_block(BasicBlock &block, StaticResults &static_results, DynamicResults* dynamic_results, bool instrument) {
    PerBlockState* pbs = block_states[&block];
    bool changed = false;

    StringRef blockname;
    if (block.hasName()) {
      blockname = block.getName();
    } else {
      blockname = "";
    }
    LLVM_DEBUG(dbgs() << "DLIM: analyzing block " << blockname << "\n");
    DEBUG_WITH_TYPE("DLIM-block-previous-state", dbgs() << "DLIM:   this block previously had " << pbs->ptrs_beg.describe() << " at beginning and " << pbs->ptrs_end.describe() << " at end\n");

    // first: if any variable is clean at the end of all of this block's
    // predecessors, then it is also clean at the beginning of this block
    if (!block.hasNPredecessors(0)) {
      auto preds = pred_begin(&block);
      const BasicBlock* firstPred = *preds;
      const PerBlockState* firstPred_pbs = block_states[firstPred];
      // we start with all of the ptr_statuses at the end of our first predecessor,
      // then merge with the ptr_statuses at the end of our other predecessors
      PointerStatuses ptr_statuses = PointerStatuses(firstPred_pbs->ptrs_end);
      DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   first predecessor has " << ptr_statuses.describe() << " at end\n");
      for (auto it = ++preds, end = pred_end(&block); it != end; ++it) {
        const BasicBlock* otherPred = *it;
        const PerBlockState* otherPred_pbs = block_states[otherPred];
        DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   next predecessor has " << otherPred_pbs->ptrs_end.describe() << " at end\n");
        ptr_statuses = PointerStatuses::merge(std::move(ptr_statuses), otherPred_pbs->ptrs_end);
      }
      // whatever's left is now the set of clean ptrs at beginning of this block
      changed |= !ptr_statuses.isEqualTo(pbs->ptrs_beg);
      pbs->ptrs_beg = std::move(ptr_statuses);
    }
    DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   at beginning of block, we now have " << pbs->ptrs_beg.describe() << "\n");

    // The current pointer statuses. This begins as `pbs.ptrs_beg`, and as we
    // go through the block, gets updated; its state at the end of the block
    // will become `pbs.ptrs_end`.
    PointerStatuses ptr_statuses = PointerStatuses(pbs->ptrs_beg);

    #define COUNT_PTR(ptr, category, kind) \
      static_results.category.kind++; \
      if (instrument) { \
        incrementGlobalCounter(dynamic_results->category.kind, (ptr)); \
      }

    // now: process each instruction
    // the only way for a dirty pointer to become clean is by being dereferenced
    // there is no way for a clean pointer to become dirty
    // so we only need to worry about pointer dereferences, and instructions
    // which produce pointers
    // (and of course we want to statically count clean/dirty loads/stores)
    for (Instruction &inst : block) {
      switch (inst.getOpcode()) {
        case Instruction::Store: {
          const StoreInst& store = cast<StoreInst>(inst);
          // first count the stored value (if it's a pointer)
          const Value* storedVal = store.getValueOperand();
          if (storedVal->getType()->isPointerTy()) {
            const PointerKind kind = ptr_statuses.getStatus(storedVal);
            switch (kind) {
              case CLEAN: COUNT_PTR(&inst, store_vals, clean) break;
              case BLEMISHED16: COUNT_PTR(&inst, store_vals, blemished16) break;
              case BLEMISHED32: COUNT_PTR(&inst, store_vals, blemished32) break;
              case BLEMISHED64: COUNT_PTR(&inst, store_vals, blemished64) break;
              case BLEMISHEDCONST: COUNT_PTR(&inst, store_vals, blemishedconst) break;
              case DIRTY: COUNT_PTR(&inst, store_vals, dirty) break;
              case UNKNOWN: COUNT_PTR(&inst, store_vals, unknown) break;
              case NOTDEFINEDYET: assert(false && "Storing a pointer with no status"); break;
              default: assert(false && "PointerKind case not handled");
            }
          }
          // next count the address
          const Value* addr = store.getPointerOperand();
          const PointerKind kind = ptr_statuses.getStatus(addr);
          switch (kind) {
            case CLEAN: COUNT_PTR(&inst, store_addrs, clean) break;
            case BLEMISHED16: COUNT_PTR(&inst, store_addrs, blemished16) break;
            case BLEMISHED32: COUNT_PTR(&inst, store_addrs, blemished32) break;
            case BLEMISHED64: COUNT_PTR(&inst, store_addrs, blemished64) break;
            case BLEMISHEDCONST: COUNT_PTR(&inst, store_addrs, blemishedconst) break;
            case DIRTY: COUNT_PTR(&inst, store_addrs, dirty) break;
            case UNKNOWN: COUNT_PTR(&inst, store_addrs, unknown) break;
            case NOTDEFINEDYET: assert(false && "Storing to pointer with no status"); break;
            default: assert(false && "PointerKind case not handled");
          }
          // now, the pointer used as an address becomes clean
          ptr_statuses.mark_clean(addr);
          break;
        }
        case Instruction::Load: {
          const LoadInst& load = cast<LoadInst>(inst);
          const Value* ptr = load.getPointerOperand();
          // first count this for static stats
          const PointerKind kind = ptr_statuses.getStatus(ptr);
          switch (kind) {
            case CLEAN: COUNT_PTR(&inst, load_addrs, clean) break;
            case BLEMISHED16: COUNT_PTR(&inst, load_addrs, blemished16) break;
            case BLEMISHED32: COUNT_PTR(&inst, load_addrs, blemished32) break;
            case BLEMISHED64: COUNT_PTR(&inst, load_addrs, blemished64) break;
            case BLEMISHEDCONST: COUNT_PTR(&inst, load_addrs, blemishedconst) break;
            case DIRTY: COUNT_PTR(&inst, load_addrs, dirty) break;
            case UNKNOWN: COUNT_PTR(&inst, load_addrs, unknown) break;
            case NOTDEFINEDYET: assert(false && "Loading from pointer with no status"); break;
            default: assert(false && "PointerKind case not handled");
          }
          // now, the pointer becomes clean
          ptr_statuses.mark_clean(ptr);

          if (load.getType()->isPointerTy()) {
            // in this case, we loaded a pointer from memory, and have to
            // worry about whether it's clean or not.
            // For now, we mark it UNKNOWN.
            ptr_statuses.mark_unknown(&load);
          }
          break;
        }
        case Instruction::Alloca: {
          // result of an alloca is a clean pointer
          ptr_statuses.mark_clean(&inst);
          break;
        }
        case Instruction::GetElementPtr: {
          GetElementPtrInst& gep = cast<GetElementPtrInst>(inst);
          const Value* input_ptr = gep.getPointerOperand();
          PointerKind input_kind = ptr_statuses.getStatus(input_ptr);
          APInt induction_offset;
          APInt initial_offset;
          bool is_induction_pattern = false;
          const LoopInfo& loopinfo = FAM.getResult<LoopAnalysis>(F);
          const PostDominatorTree& pdtree = FAM.getResult<PostDominatorTreeAnalysis>(F);
          if (isOffsetAnInductionPattern(gep, DL, loopinfo, pdtree, &induction_offset, &initial_offset)
            && induction_offset.isNonNegative()
            && initial_offset.isNonNegative())
          {
            is_induction_pattern = true;
            if (initial_offset.sge(induction_offset)) {
              induction_offset = std::move(initial_offset);
            }
            LLVM_DEBUG(dbgs() << "DLIM:   found an induction GEP with offset effectively constant " << induction_offset << "\n");
          }
          bool offsetIsNonzeroConstant;
          ptr_statuses.mark_as(&gep, classifyGEPResult(gep, input_kind, DL, trustLLVMStructTypes, is_induction_pattern ? &induction_offset : NULL, &offsetIsNonzeroConstant));
          // if we added a nonzero constant to a pointer, count that for stats purposes
          if (offsetIsNonzeroConstant) {
            switch (input_kind) {
              case CLEAN: COUNT_PTR(&gep, pointer_arith_const, clean) break;
              case BLEMISHED16: COUNT_PTR(&gep, pointer_arith_const, blemished16) break;
              case BLEMISHED32: COUNT_PTR(&gep, pointer_arith_const, blemished32) break;
              case BLEMISHED64: COUNT_PTR(&gep, pointer_arith_const, blemished64) break;
              case BLEMISHEDCONST: COUNT_PTR(&gep, pointer_arith_const, blemishedconst) break;
              case DIRTY: COUNT_PTR(&gep, pointer_arith_const, dirty) break;
              case UNKNOWN: COUNT_PTR(&gep, pointer_arith_const, unknown) break;
              case NOTDEFINEDYET: assert(false && "GEP on a pointer with no status"); break;
              default: assert(false && "PointerKind case not handled");
            }
          }
          break;
        }
        case Instruction::BitCast: {
          const BitCastInst& bitcast = cast<BitCastInst>(inst);
          if (bitcast.getType()->isPointerTy()) {
            const Value* input_ptr = bitcast.getOperand(0);
            ptr_statuses.mark_as(&bitcast, ptr_statuses.getStatus(input_ptr));
          }
          break;
        }
        case Instruction::AddrSpaceCast: {
          const Value* input_ptr = inst.getOperand(0);
          ptr_statuses.mark_as(&inst, ptr_statuses.getStatus(input_ptr));
          break;
        }
        case Instruction::Select: {
          const SelectInst& select = cast<SelectInst>(inst);
          if (select.getType()->isPointerTy()) {
            // output is clean if both inputs are clean; etc
            const Value* true_input = select.getTrueValue();
            const Value* false_input = select.getFalseValue();
            const PointerKind true_kind = ptr_statuses.getStatus(true_input);
            const PointerKind false_kind = ptr_statuses.getStatus(false_input);
            ptr_statuses.mark_as(&select, merge(true_kind, false_kind));
          }
          break;
        }
        case Instruction::PHI: {
          const PHINode& phi = cast<PHINode>(inst);
          if (phi.getType()->isPointerTy()) {
            // phi: result kind is the merger of the kinds of all the inputs
            // in their corresponding blocks
            PointerKind merged_kind = CLEAN;
            for (const Use& use : phi.incoming_values()) {
              const BasicBlock* bb = phi.getIncomingBlock(use);
              auto& ptr_statuses_end_of_bb = block_states[bb]->ptrs_end;
              const Value* value = use.get();
              merged_kind = merge(merged_kind, ptr_statuses_end_of_bb.getStatus(value));
            }
            ptr_statuses.mark_as(&phi, merged_kind);
          }
          break;
        }
        case Instruction::IntToPtr: {
          // count this for stats, and then mark it as `inttoptr_kind`
          static_results.inttoptrs++;
          if (instrument) {
            incrementGlobalCounter(dynamic_results->inttoptrs, &inst);
          }
          ptr_statuses.mark_as(&inst, inttoptr_kind);
          break;
        }
        case Instruction::Call:
        case Instruction::CallBr:
        case Instruction::Invoke:
        // all three of these are instructions which call functions, and we
        // handle them the same
        {
          const CallBase& call = cast<CallBase>(inst);
          // count call arguments for static stats
          for (const Use& arg : call.args()) {
            const Value* value = arg.get();
            if (value->getType()->isPointerTy()) {
              const PointerKind kind = ptr_statuses.getStatus(value);
              switch (kind) {
                case CLEAN: COUNT_PTR(&inst, passed_ptrs, clean) break;
                case BLEMISHED16: COUNT_PTR(&inst, passed_ptrs, blemished16) break;
                case BLEMISHED32: COUNT_PTR(&inst, passed_ptrs, blemished32) break;
                case BLEMISHED64: COUNT_PTR(&inst, passed_ptrs, blemished64) break;
                case BLEMISHEDCONST: COUNT_PTR(&inst, passed_ptrs, blemishedconst) break;
                case DIRTY: COUNT_PTR(&inst, passed_ptrs, dirty) break;
                case UNKNOWN: COUNT_PTR(&inst, passed_ptrs, unknown) break;
                case NOTDEFINEDYET: assert(false && "Call argument is a pointer with no status"); break;
                default: assert(false && "PointerKind case not handled");
              }
            }
          }
          // now classify the returned pointer, if the return value is a pointer
          if (call.getType()->isPointerTy()) {
            // If this is an allocating call (eg, a call to `malloc`), then the
            // returned pointer is CLEAN
            if (isAllocatingCall(call)) {
              ptr_statuses.mark_clean(&call);
            } else {
              // For now, mark pointers returned from other calls as UNKNOWN
              ptr_statuses.mark_unknown(&call);
            }
          }
          break;
        }
        case Instruction::ExtractValue: {
          // this gets a pointer out of a field of a first-class struct (not a
          // pointer-to-a-struct, so ptr_statuses doesn't have any information
          // about this struct).
          // the question is where did the struct come from.
          // As I see it, probably either (a) we created the struct with
          // insertvalue - in which case, ideally we'd give this result
          // the same PointerKind as the original pointer which was inserted;
          // or (b) we loaded the struct from memory (?), in which case we
          // should just mark the result UNKNOWN per our current assumptions.
          // So for now, we'll just mark UNKNOWN and move on
          ptr_statuses.mark_unknown(&inst);
          break;
        }
        case Instruction::ExtractElement: {
          // same comments apply as for ExtractValue, basically
          ptr_statuses.mark_unknown(&inst);
          break;
        }
        case Instruction::Ret: {
          const ReturnInst& ret = cast<ReturnInst>(inst);
          const Value* retval = ret.getReturnValue();
          if (retval && retval->getType()->isPointerTy()) {
            const PointerKind kind = ptr_statuses.getStatus(retval);
            switch (kind) {
              case CLEAN: COUNT_PTR(&inst, returned_ptrs, clean) break;
              case BLEMISHED16: COUNT_PTR(&inst, returned_ptrs, blemished16) break;
              case BLEMISHED32: COUNT_PTR(&inst, returned_ptrs, blemished32) break;
              case BLEMISHED64: COUNT_PTR(&inst, returned_ptrs, blemished64) break;
              case BLEMISHEDCONST: COUNT_PTR(&inst, returned_ptrs, blemishedconst) break;
              case DIRTY: COUNT_PTR(&inst, returned_ptrs, dirty) break;
              case UNKNOWN: COUNT_PTR(&inst, returned_ptrs, unknown) break;
              case NOTDEFINEDYET: assert(false && "Returning a pointer with no status"); break;
              default: assert(false && "PointerKind case not handled");
            }
          }
          break;
        }
        default:
          if (inst.getType()->isPointerTy()) {
            errs() << "Encountered a pointer-producing instruction which we don't have a case for. Does it produce a clean or dirty pointer?\n";
            inst.dump();
          }
          break;
      }
    }

    // Now that we've processed all the instructions, we have the final
    // statuses of pointers as of the end of the block
    DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   at end of block, we now have " << ptr_statuses.describe() << "\n");
    const bool end_changed = !ptr_statuses.isEqualTo(pbs->ptrs_end);
    if (end_changed) {
      DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   this was a change\n");
    }
    changed |= end_changed;
    pbs->ptrs_end = std::move(ptr_statuses);
    return changed;
  }

  DynamicResults initializeDynamicResults() {
    DynamicCounts load_addrs = initializeDynamicCounts("__DLIM_load_addrs");
    DynamicCounts store_addrs = initializeDynamicCounts("__DLIM_store_addrs");
    DynamicCounts store_vals = initializeDynamicCounts("__DLIM_store_vals");
    DynamicCounts passed_ptrs = initializeDynamicCounts("__DLIM_passed_ptrs");
    DynamicCounts returned_ptrs = initializeDynamicCounts("__DLIM_returned_ptrs");
    DynamicCounts pointer_arith_const = initializeDynamicCounts("__DLIM_pointer_arith_const");
    Constant* inttoptrs = findOrCreateGlobalCounter("__DLIM_inttoptrs");
    return DynamicResults { load_addrs, store_addrs, store_vals, passed_ptrs, returned_ptrs, pointer_arith_const, inttoptrs };
  }

  DynamicCounts initializeDynamicCounts(StringRef thingToCount) {
    Constant* clean = findOrCreateGlobalCounter(thingToCount + "_clean");
    Constant* blemished16 = findOrCreateGlobalCounter(thingToCount + "_blemished16");
    Constant* blemished32 = findOrCreateGlobalCounter(thingToCount + "_blemished32");
    Constant* blemished64 = findOrCreateGlobalCounter(thingToCount + "_blemished64");
    Constant* blemishedconst = findOrCreateGlobalCounter(thingToCount + "_blemishedconst");
    Constant* dirty = findOrCreateGlobalCounter(thingToCount + "_dirty");
    Constant* unknown = findOrCreateGlobalCounter(thingToCount + "_unknown");
    return DynamicCounts { clean, blemished16, blemished32, blemished64, blemishedconst, dirty, unknown };
  }

  Constant* findOrCreateGlobalCounter(Twine Name) {
    // https://github.com/banach-space/llvm-tutor/blob/0d2864d19b90fbcc31cea530ec00215405271e40/lib/DynamicCallCounter.cpp
    Module* mod = F.getParent();
    LLVMContext& ctx = mod->getContext();
    Type* i64ty = IntegerType::getInt64Ty(ctx);

    Constant* global = mod->getOrInsertGlobal(Name.str(), i64ty);
    GlobalVariable* gv = cast<GlobalVariable>(global);
    if (!gv->hasInitializer()) {
      gv->setLinkage(GlobalValue::PrivateLinkage);
      gv->setAlignment(MaybeAlign(8));
      gv->setInitializer(ConstantInt::get(ctx, APInt(/* bits = */ 64, /* val = */ 0)));
    }

    return global;
  }

  // Inject an instruction sequence to increment the given global counter, right
  // before the given instruction
  void incrementGlobalCounter(Constant* GlobalCounter, Instruction* BeforeInst) {
    IRBuilder<> Builder(BeforeInst);
    Type* i64ty = Builder.getInt64Ty();
    LoadInst* loaded = Builder.CreateLoad(i64ty, GlobalCounter);
    Value* incremented = Builder.CreateAdd(Builder.getInt64(1), loaded);
    Builder.CreateStore(incremented, GlobalCounter);
  }

  void addDynamicResultsPrint(DynamicResults& dynamic_results, DynamicPrintType print_type) {
    // https://github.com/banach-space/llvm-tutor/blob/0d2864d19b90fbcc31cea530ec00215405271e40/lib/DynamicCallCounter.cpp
    Module* mod = F.getParent();
    LLVMContext& ctx = mod->getContext();

    // if this function already exists in the module, assume we've already added
    // the print
    if (mod->getFunction("__DLIM_output_wrapper")) {
      return;
    }

    std::string output = "";
    output += "================\n";
    output += "DLIM dynamic counts for " + mod->getName().str() + ":\n";
    output += "================\n";
    output += "Loads with clean addr: %llu\n";
    output += "Loads with blemished16 addr: %llu\n";
    output += "Loads with blemished32 addr: %llu\n";
    output += "Loads with blemished64 addr: %llu\n";
    output += "Loads with blemishedconst addr: %llu\n";
    output += "Loads with dirty addr: %llu\n";
    output += "Loads with unknown addr: %llu\n";
    output += "Stores with clean addr: %llu\n";
    output += "Stores with blemished16 addr: %llu\n";
    output += "Stores with blemished32 addr: %llu\n";
    output += "Stores with blemished64 addr: %llu\n";
    output += "Stores with blemishedconst addr: %llu\n";
    output += "Stores with dirty addr: %llu\n";
    output += "Stores with unknown addr: %llu\n";
    output += "Storing a clean ptr to mem: %llu\n";
    output += "Storing a blemished16 ptr to mem: %llu\n";
    output += "Storing a blemished32 ptr to mem: %llu\n";
    output += "Storing a blemished64 ptr to mem: %llu\n";
    output += "Storing a blemishedconst ptr to mem: %llu\n";
    output += "Storing a dirty ptr to mem: %llu\n";
    output += "Storing an unknown ptr to mem: %llu\n";
    output += "Passing a clean ptr to a func: %llu\n";
    output += "Passing a blemished16 ptr to a func: %llu\n";
    output += "Passing a blemished32 ptr to a func: %llu\n";
    output += "Passing a blemished64 ptr to a func: %llu\n";
    output += "Passing a blemishedconst ptr to a func: %llu\n";
    output += "Passing a dirty ptr to a func: %llu\n";
    output += "Passing an unknown ptr to a func: %llu\n";
    output += "Returning a clean ptr from a func: %llu\n";
    output += "Returning a blemished16 ptr from a func: %llu\n";
    output += "Returning a blemished32 ptr from a func: %llu\n";
    output += "Returning a blemished64 ptr from a func: %llu\n";
    output += "Returning a blemishedconst ptr from a func: %llu\n";
    output += "Returning a dirty ptr from a func: %llu\n";
    output += "Returning an unknown ptr from a func: %llu\n";
    output += "Nonzero constant pointer arithmetic on a clean ptr: %llu\n";
    output += "Nonzero constant pointer arithmetic on a blemished16 ptr: %llu\n";
    output += "Nonzero constant pointer arithmetic on a blemished32 ptr: %llu\n";
    output += "Nonzero constant pointer arithmetic on a blemished64 ptr: %llu\n";
    output += "Nonzero constant pointer arithmetic on a blemishedconst ptr: %llu\n";
    output += "Nonzero constant pointer arithmetic on a dirty ptr: %llu\n";
    output += "Nonzero constant pointer arithmetic on an unknown ptr: %llu\n";
    output += "Producing a ptr from inttoptr: %llu\n";
    output += "\n";

    // Inject a global variable to hold the output string
    Constant* OutputStr = createGlobalConstStr(mod, "__DLIM_output_str", output.c_str());

    // Create a void function which calls printf() or fprintf() to print the
    // output
    Type* i8ty = IntegerType::getInt8Ty(ctx);
    Type* i8StarTy = PointerType::getUnqual(i8ty);
    Type* i32ty = IntegerType::getInt32Ty(ctx);
    Type* i32StarTy = PointerType::getUnqual(i32ty);
    Type* i64ty = IntegerType::getInt64Ty(ctx);
    FunctionType* WrapperTy = FunctionType::get(Type::getVoidTy(ctx), {}, false);
    Function* Wrapper_func = cast<Function>(mod->getOrInsertFunction("__DLIM_output_wrapper", WrapperTy).getCallee());
    Wrapper_func->setLinkage(GlobalValue::PrivateLinkage);
    BasicBlock* EntryBlock = BasicBlock::Create(ctx, "entry", Wrapper_func);
    IRBuilder<> Builder(EntryBlock);

    if (print_type == STDOUT) {
      // call printf()
      FunctionType* PrintfTy = FunctionType::get(i32ty, i8StarTy, /* IsVarArgs = */ true);
      FunctionCallee Printf = mod->getOrInsertFunction("printf", PrintfTy);
      //Function* Printf_func = cast<Function>(Printf.getCallee());
      //Printf_func->setDoesNotThrow();
      //Printf_func->addParamAttr(0, Attribute::NoCapture);
      //Printf_func->addParamAttr(0, Attribute::ReadOnly);
      Builder.CreateCall(Printf, {
        Builder.CreatePointerCast(OutputStr, i8StarTy),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.clean),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.inttoptrs),
      });
      Builder.CreateRetVoid();
    } else if (print_type == TOFILE) {
      // create strings for arguments to mkdir, fopen, and perror
      auto modNameNoDotDot = regexSubAll(Regex("\\.\\./"), "", mod->getName());
      auto modNameWithDots = regexSubAll(Regex("/"), ".", modNameNoDotDot);
      auto file_str = "./dlim_dynamic_counts/" + modNameWithDots;
      Constant* DirStr = createGlobalConstStr(mod, "__DLIM_dir_str", "./dlim_dynamic_counts");
      Constant* FileStr = createGlobalConstStr(mod, "__DLIM_file_str", file_str.c_str());
      Constant* ModeStr = createGlobalConstStr(mod, "__DLIM_mode_str", "a");
      Constant* PerrorStr = createGlobalConstStr(mod, "__DLIM_perror_str", ("Failed to open " + file_str).c_str());
      // call mkdir
      FunctionType* MkdirTy = FunctionType::get(i32ty, {i8StarTy, i32ty}, /* IsVarArgs = */ false);
      FunctionCallee Mkdir = mod->getOrInsertFunction("mkdir", MkdirTy);
      //Function* Mkdir_func = cast<Function>(Mkdir.getCallee());
      //Mkdir_func->addParamAttr(0, Attribute::NoCapture);
      //Mkdir_func->addParamAttr(0, Attribute::ReadOnly);
      Value* Mkdir_ret = Builder.CreateCall(Mkdir, {
        Builder.CreatePointerCast(DirStr, i8StarTy),
        Builder.getInt32(/* octal */ 0777),
      });
      // check for error from mkdir
      Value* cond = Builder.CreateICmpEQ(Mkdir_ret, Builder.getInt32(0));
      BasicBlock* ErrorBB = BasicBlock::Create(ctx, "error", Wrapper_func);
      BasicBlock* NoErrorBB = BasicBlock::Create(ctx, "noerror", Wrapper_func);
      Builder.CreateCondBr(cond, NoErrorBB, ErrorBB);
      // the case where mkdir returns error
      // see if errno is EEXIST (17), and if so, ignore the error.
      // otherwise return early and don't print anything.
      Builder.SetInsertPoint(ErrorBB);
      FunctionType* ErrnoTy = FunctionType::get(i32StarTy, {}, false);
      FunctionCallee Errno_callee = mod->getOrInsertFunction("__errno_location", ErrnoTy);
      //Function* Errno_func = cast<Function>(Errno_callee.getCallee());
      //Errno_func->setDoesNotAccessMemory();
      //Errno_func->setDoesNotThrow();
      //Errno_func->setWillReturn();
      Value* errno_addr = Builder.CreateCall(Errno_callee, {});
      Value* errno_val = Builder.CreateLoad(i32ty, errno_addr);
      cond = Builder.CreateICmpEQ(errno_val, Builder.getInt32(17));
      BasicBlock* JustReturnBB = BasicBlock::Create(ctx, "justreturn", Wrapper_func);
      Builder.CreateCondBr(cond, NoErrorBB, JustReturnBB);
      Builder.SetInsertPoint(JustReturnBB);
      Builder.CreateRetVoid();
      // the case where mkdir succeeds (or where we got EEXIST and ignored it -
      // in either case the directory now exists).
      // Call fopen
      Builder.SetInsertPoint(NoErrorBB);
      StructType* FileTy = StructType::create(ctx, "struct._IO_FILE");
      PointerType* FileStarTy = PointerType::getUnqual(FileTy);
      FunctionType* FopenTy = FunctionType::get(FileStarTy, {i8StarTy, i8StarTy}, false);
      FunctionCallee Fopen = mod->getOrInsertFunction("fopen", FopenTy);
      //Function* Fopen_func = cast<Function>(Fopen.getCallee());
      //Fopen_func->addParamAttr(0, Attribute::NoCapture);
      //Fopen_func->addParamAttr(0, Attribute::ReadOnly);
      //Fopen_func->addParamAttr(1, Attribute::NoCapture);
      //Fopen_func->addParamAttr(1, Attribute::ReadOnly);
      Value* file_handle = Builder.CreateCall(Fopen, {
        Builder.CreatePointerCast(FileStr, i8StarTy),
        Builder.CreatePointerCast(ModeStr, i8StarTy),
      });
      // check for error from fopen
      cond = Builder.CreateIsNull(file_handle);
      BasicBlock* WriteBB = BasicBlock::Create(ctx, "write", Wrapper_func);
      BasicBlock* FopenFailed = BasicBlock::Create(ctx, "fopenfailed", Wrapper_func);
      Builder.CreateCondBr(cond, FopenFailed, WriteBB);
      Builder.SetInsertPoint(FopenFailed);
      FunctionType* PerrorTy = FunctionType::get(Type::getVoidTy(ctx), {i8StarTy}, false);
      FunctionCallee Perror = mod->getOrInsertFunction("perror", PerrorTy);
      Builder.CreateCall(Perror, {Builder.CreatePointerCast(PerrorStr, i8StarTy)});
      Builder.CreateRetVoid();
      // and, actually write to file
      Builder.SetInsertPoint(WriteBB);
      FunctionType* FprintfTy = FunctionType::get(i32ty, {FileStarTy, i8StarTy}, /* IsVarArgs = */ true);
      FunctionCallee Fprintf = mod->getOrInsertFunction("fprintf", FprintfTy);
      Builder.CreateCall(Fprintf, {
        file_handle,
        Builder.CreatePointerCast(OutputStr, i8StarTy),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.clean),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemished16),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemished32),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemished64),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.blemishedconst),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.pointer_arith_const.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.inttoptrs),
      });
      FunctionType* FcloseTy = FunctionType::get(i32ty, {FileStarTy}, false);
      FunctionCallee Fclose = mod->getOrInsertFunction("fclose", FcloseTy);
      Builder.CreateCall(Fclose, {file_handle});
      Builder.CreateBr(JustReturnBB);
    } else {
      assert(false && "unexpected print_type\n");
    }

    // Inject this wrapper function into the GlobalDtors for the module
    appendToGlobalDtors(*mod, Wrapper_func, /* Priority = */ 0);
  }
};

PreservedAnalyses StaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, true, CLEAN);
  DLIMAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // StaticDLIMPass only analyzes the IR and doesn't make any changes, so all
  // analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses ParanoidStaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, false, DIRTY);
  DLIMAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // ParanoidStaticDLIMPass only analyzes the IR and doesn't make any changes,
  // so all analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses DynamicDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // Don't do any analysis or instrumentation on the special function __DLIM_output_wrapper
  if (F.getName() == "__DLIM_output_wrapper") {
    return PreservedAnalyses::all();
  }

  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, true, CLEAN);
  analysis.run();
  analysis.instrument(DLIMAnalysis::DynamicPrintType::TOFILE);

  // For now we conservatively just tell LLVM that no analyses are preserved.
  // It seems that many existing LLVM passes also just use
  // PreservedAnalyses::none() when they make any change, so we assume this is
  // reasonable.
  return PreservedAnalyses::none();
}

PreservedAnalyses DynamicStdoutDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // Don't do any analysis or instrumentation on the special function __DLIM_output_wrapper
  if (F.getName() == "__DLIM_output_wrapper") {
    return PreservedAnalyses::all();
  }

  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, true, CLEAN);
  analysis.run();
  analysis.instrument(DLIMAnalysis::DynamicPrintType::STDOUT);

  // For now we conservatively just tell LLVM that no analyses are preserved.
  // It seems that many existing LLVM passes also just use
  // PreservedAnalyses::none() when they make any change, so we assume this is
  // reasonable.
  return PreservedAnalyses::none();
}

/// Classify the `PointerKind` of the result of the given `gep`, assuming that its
/// input pointer is `input_kind`.
/// This looks only at the `GetElementPtrInst` itself, and thus does not try to
/// do any loop induction reasoning etc (that is done elsewhere).
/// Think of this as giving the raw/default result for the `gep`.
///
/// `override_constant_offset`: if this is not NULL, then ignore the GEP's indices
/// and classify it as if the offset were the given compile-time constant.
///
/// `offsetIsNonzeroConstant`: an output parameter. This function will write to
/// `offsetIsNonzeroConstant` indicating if the total offset of the GEP was considered
/// a nonzero constant or not. (If `override_constant_offset` is non-NULL, and
/// nonzero, this will always be `true`, of course.)
static PointerKind classifyGEPResult(
  const GetElementPtrInst &gep,
  const PointerKind input_kind,
  const DataLayout &DL,
  const bool trustLLVMStructTypes,
  const APInt* override_constant_offset,
  /* output */ bool* offsetIsNonzeroConstant
) {
  bool offsetIsConstant = false;
  // `offset` is only valid if `offsetIsConstant`
  APInt offset = APInt(/* bits = */ 64, /* val = */ 0);
  if (override_constant_offset == NULL) {
    offsetIsConstant = gep.accumulateConstantOffset(DL, offset);
  } else {
    offsetIsConstant = true;
    offset = *override_constant_offset;
  }

  if (gep.hasAllZeroIndices()) {
    // result of a GEP with all zeroes as indices, is the same as the input pointer.
    assert(offsetIsConstant && offset == APInt(/* bits = */ 64, /* val = */ 0) && "If all indices are constant 0, then the total offset should be constant 0");
    *offsetIsNonzeroConstant = false; // it's a zero constant
    return input_kind;
  }
  if (trustLLVMStructTypes && input_kind == CLEAN && areAllIndicesTrustworthy(gep)) {
    // nonzero offset, but "trustworthy" offset, from a clean pointer.
    // The resulting pointer is clean.
    *offsetIsNonzeroConstant = false; // we consider this a "zero" constant. For this purpose.
    return CLEAN;
  }

  // if we get here, we don't have a zero constant offset. Either it's a nonzero constant,
  // or a nonconstant.
  *offsetIsNonzeroConstant = offsetIsConstant;
  if (offsetIsConstant) {
    switch (input_kind) {
      case CLEAN: {
        // This GEP adds a constant but nonzero amount to a CLEAN
        // pointer. The result is some flavor of BLEMISHED depending
        // on how far the pointer arithmetic goes.
        if (offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          return BLEMISHED16;
        } else if (offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
          return BLEMISHED32;
        } else if (offset.ule(APInt(/* bits = */ 64, /* val = */ 64))) {
          return BLEMISHED64;
        } else {
          // offset is constant, but larger than 64 bytes
          return BLEMISHEDCONST;
        }
        break;
      }
      case BLEMISHED16: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHED16 pointer. The result is some flavor of BLEMISHED
        // depending on how far the pointer arithmetic goes.
        if (offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          // Conservatively, the total offset can't exceed 32
          return BLEMISHED32;
        } else if (offset.ule(APInt(/* bits = */ 64, /* val = */ 48))) {
          // Conservatively, the total offset can't exceed 64
          return BLEMISHED64;
        } else {
          // offset is constant, but may be larger than 64 bytes
          return BLEMISHEDCONST;
        }
        break;
      }
      case BLEMISHED32: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHED32 pointer. The result is some flavor of BLEMISHED
        // depending on how far the pointer arithmetic goes.
        if (offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
          // Conservatively, the total offset can't exceed 64
          return BLEMISHED64;
        } else {
          // offset is constant, but may be larger than 64 bytes
          return BLEMISHEDCONST;
        }
        break;
      }
      case BLEMISHED64: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHED64 pointer. The result is BLEMISHEDCONST, as we
        // can't prove the total constant offset remains 64 or less.
        return BLEMISHEDCONST;
        break;
      }
      case BLEMISHEDCONST: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHEDCONST pointer. The result is still BLEMISHEDCONST,
        // as the total offset is still a constant.
        return BLEMISHEDCONST;
        break;
      }
      case DIRTY: {
        // result of a GEP with any nonzero indices, on a DIRTY or
        // UNKNOWN pointer, is always DIRTY.
        return DIRTY;
        break;
      }
      case UNKNOWN: {
        // result of a GEP with any nonzero indices, on a DIRTY or
        // UNKNOWN pointer, is always DIRTY.
        return DIRTY;
        break;
      }
      case NOTDEFINEDYET: {
        assert(false && "GEP on a pointer with no status");
        break;
      }
      default: {
        assert(false && "Missing PointerKind case");
        break;
      }
    }
  } else {
    // offset is not constant; so, result is dirty
    return DIRTY;
  }
}

/// Print a description of the pointers in `ptrs` to `out`. `desc` is an
/// adjective describing the pointers (e.g., "clean")
static void describePointerList(const SmallVector<const Value*, 8>& ptrs, std::ostringstream& out, StringRef desc) {
  std::string desc_str = desc.str();
  switch (ptrs.size()) {
    case 0: {
      out << "0 " << desc_str << " ptrs";
      break;
    }
    case 1: {
      const Value* ptr = ptrs[0];
      out << "1 " << desc_str << " ptr (" << ptr->getNameOrAsOperand() << ")";
      break;
    }
    default: {
      if (ptrs.size() <= 8) {
        out << ptrs.size() << " " << desc_str << " ptrs (";
        for (const Value* ptr : ptrs) {
          out << ptr->getNameOrAsOperand() << ", ";
        }
        out << ")";
      } else {
        out << ptrs.size() << " " << desc_str << " ptrs";
      }
      break;
    }
  }
}

static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep) {
  DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "Analyzing the following gep:\n");
  DEBUG_WITH_TYPE("DLIM-trustworthy-indices", gep.dump());
  Type* current_ty = gep.getPointerOperandType();
  SmallVector<Constant*, 8> seen_indices;
  for (const Use& idx : gep.indices()) {
    if (!current_ty) {
      LLVM_DEBUG(dbgs() << "current_ty is null - probably getIndexedType() returned null\n");
      return false;
    }
    if (ConstantInt* c = dyn_cast<ConstantInt>(idx.get())) {
      DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "Encountered constant index " << c->getSExtValue() << "\n");
      DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "Current ty is " << *current_ty << "\n");
      seen_indices.push_back(cast<Constant>(c));
      if (c->isZero()) {
        // zero is always trustworthy
        DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "zero is always trustworthy\n");
      } else {
        // constant, nonzero index
        if (seen_indices.size() == 1) {
          // the first time is just selecting the element of the implied array.
          DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "indexing into an implicit array is not trustworthy\n");
          return false;
        }
        const PointerType* current_ty_as_ptrtype = cast<const PointerType>(current_ty);
        const Type* current_pointee_ty = current_ty_as_ptrtype->getElementType();
        DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "Current pointee ty is " << *current_pointee_ty << "\n");
        if (current_pointee_ty->isStructTy()) {
          // trustworthy
          DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "indexing into a struct ty is trustworthy\n");
        } else if (current_pointee_ty->isArrayTy()) {
          // not trustworthy
          DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "indexing into an array ty is not trustworthy\n");
          return false;
        } else {
          // implicit array type. e.g., indexing into an i32*.
          DEBUG_WITH_TYPE("DLIM-trustworthy-indices", dbgs() << "indexing into an implicit array is not trustworthy\n");
          return false;
        }
      }
    } else {
      // any nonconstant index? then return false
      return false;
    }
    if (seen_indices.size() == 1) {
      // the first time, we don't update `current_ty`, because the first GEP index
      // is just selecting the element of the implied array
    } else {
      ArrayRef<Constant*> seen_indices_arrayref = ArrayRef<Constant*>(seen_indices);
      current_ty = GetElementPtrInst::getIndexedType(gep.getPointerOperandType(), seen_indices_arrayref);
    }
  }
  // if we get here without finding a non-trustworthy index, then we're all good
  return true;
}

/// Is the offset of the given GEP an induction pattern?
/// This is looking for a pretty specific pattern for GEPs inside loops, which
/// we can optimize checks for.
///
/// If this function identifies an induction pattern, it returns `true` and
/// writes to `out_induction_offset` and `out_initial_offset`.
/// The GEP has effectively the offset `out_initial_offset` during the first
/// loop iteration, and the offset is incremented by `out_induction_offset` each
/// subsequent loop iteration.
static bool isOffsetAnInductionPattern(
  const GetElementPtrInst &gep,
  const DataLayout &DL,
  const LoopInfo& loopinfo,
  const PostDominatorTree& pdtree,
  /* output */ APInt* out_induction_offset,
  /* output */ APInt* out_initial_offset
) {
  DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:   Checking the following gep for induction:\n");
  DEBUG_WITH_TYPE("DLIM-loop-induction", gep.dump());
  if (gep.getNumIndices() != 1) return false; // we only handle simple cases for now
  for (const Use& idx_as_use : gep.indices()) {
    // note that this for loop goes exactly one iteration, due to the check above.
    // `idx` will be the one index of the GEP.
    const Value* idx = idx_as_use.get();
    APInt initial_val;
    APInt induction_increment;
    const Value* val;
    APInt constant;
    bool success = false;
    if (isInductionVar(idx, &induction_increment, &initial_val)) {
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     GEP single index is an induction var\n");
      success = true;
    } else if (isValuePlusConstant(idx, &val, &constant)) {
      // GEP index is `val` plus `constant`. Let's see if `val` is itself an
      // induction variable. This can happen if we are, say, accessing
      // `arr[k+1]` in a loop over `k`
      if (isInductionVar(val, &induction_increment, &initial_val)) {
        DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     GEP single index is an induction var plus a constant " << constant << "\n");
        initial_val = initial_val + constant;  // the first iteration, it's the initial value of the induction variable plus the constant it's always modified by
        // but the induction increment doesn't care about the constant modification
        success = true;
      }
    }
    if (!success) return false;
    // If we get to here, we've found an induction pattern, described by
    // `initial_val` and `induction_increment`.
    // However, we still need to ensure that the pointer produced by the GEP
    // actually is guaranteed to be dereferenced during every iteration
    // (resetting it to clean) -- otherwise we can't use the induction reasoning.
    // For now, we conservatively require a stronger property:
    //   - the pointer must be used for one or more load/stores
    //   - at least one of those load/stores must have both of these properties:
    //     - postdominates the GEP
    //     - is inside the loop
    success = false;
    const Loop* geploop = loopinfo.getLoopFor(gep.getParent());
    assert(geploop && "GEP should be in a loop");
    for (const User* user : gep.users()) {
      if (isa<LoadInst>(user) || isa<StoreInst>(user)) {
        const Instruction* inst = cast<Instruction>(user);
        // first check: the load or store postdominates the GEP
        if (!pdtree.dominates(inst, &gep)) {
          assert(inst->getParent() != gep.getParent());
          continue;
        }
        // second check: the load or store must execute during each loop iteration.
        // I.e., the load or store must execute between each two consecutive
        // executions of the GEP.
        // I.e., the load or store is present on all paths from the GEP to itself.
        // For now, we approximate this as, the load/store and the GEP have the same
        // result for "what is the innermost loop you live in". I think that works.
        // At least, it works for our current regression tests.
        if (loopinfo.getLoopFor(inst->getParent()) == geploop) {
          success = true;
          break;
        }
      }
    }
    if (success) {
      // we have the constant initial_val and induction_increment.
      // but we still need to scale them by the size of the underlying array
      // elements, in order to get the GEP offsets.
      auto element_size = DL.getTypeStoreSize(gep.getSourceElementType()).getFixedSize();
      assert(element_size > 0);
      APInt ap_element_size = APInt(/* bits = */ 64, /* val = */ element_size);
      *out_initial_offset = initial_val * ap_element_size;
      *out_induction_offset = induction_increment * ap_element_size;
      return true;
    } else {
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     but failed the dereference-inside-loop check\n");
      return false;
    }
  }
  assert(false && "should return from inside the for loop");
}

/// Is the given `val` an induction variable?
/// Here, "induction variable" is narrowly defined as:
///     a PHI between a constant (initial value) and a variable (induction)
///     equal to itself plus or minus a constant
///
/// If so, returns `true` and writes to `out_induction_increment` and
/// `out_initial_val`.
/// `val` is equal to `out_initial_val` on the first loop iteration, and is
/// incremented by `out_induction_increment` each iteration.
static bool isInductionVar(
  const Value* val,
  /* output */ APInt* out_induction_increment,
  /* output */ APInt* out_initial_val
) {
  if (const PHINode* phi = dyn_cast<PHINode>(val)) {
    bool found_initial_val = false;
    bool found_induction_increment = false;
    APInt initial_val;
    APInt induction_increment;
    const Value* nonconstant_value;
    for (const Use& use : phi->incoming_values()) {
      const Value* phi_val = use.get();
      if (const Constant* phi_val_const = dyn_cast<Constant>(phi_val)) {
        if (found_initial_val) {
          // two constants in this phi. For now, this isn't a pattern we'll consider for induction.
          return false;
        }
        found_initial_val = true;
        initial_val = phi_val_const->getUniqueInteger();
      } else if (isValuePlusConstant(phi_val, &nonconstant_value, &induction_increment)) {
        if (found_induction_increment) {
          // two non-constants in this phi. For now, this isn't a pattern we'll consider for induction.
          return false;
        }
        // we're looking for the case where we are adding or subbing a
        // constant from the same value
        if (nonconstant_value == val) {
          found_induction_increment = true;
        }
      }
    }
    if (found_initial_val && found_induction_increment) {
      initial_val = initial_val.sextOrSelf(64);
      induction_increment = induction_increment.sextOrSelf(64);
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     Found an induction var, initial " << initial_val << " and induction " << induction_increment << "\n");
      *out_initial_val = std::move(initial_val);
      *out_induction_increment = std::move(induction_increment);
      return true;
    } else {
      return false;
    }
  } else {
    return false;
  }
}

/// Is the given `val` defined as some other `Value` plus/minus a constant?
/// If so, returns `true`, and writes to `out_val` and `out_const`, where `val`
/// is defined as `out_val` plus `out_const`
static bool isValuePlusConstant(
  const Value* val,
  /* output */ const Value** out_val,
  /* output */ APInt* out_const
) {
  if (const BinaryOperator* bop = dyn_cast<BinaryOperator>(val)) {
    switch (bop->getOpcode()) {
      case Instruction::Add:
      case Instruction::Sub:
      {
        bool found_constant_operand = false;
        bool found_nonconstant_operand = false;
        APInt constant_val;
        const Value* nonconstant_val;
        for (const Value* op : bop->operand_values()) {
          if (const Constant* op_const = dyn_cast<Constant>(op)) {
            if (found_constant_operand) {
              // adding or subbing two constants. Shouldn't be valid LLVM, but we'll fail gracefully.
              return false;
            }
            found_constant_operand = true;
            constant_val = op_const->getUniqueInteger();
            if (bop->getOpcode() == Instruction::Sub) {
              constant_val = -constant_val;
            }
          } else {
            if (found_nonconstant_operand) {
              // two nonconstant operands
              return false;
            }
            found_nonconstant_operand = true;
            nonconstant_val = op;
          }
        }
        if (found_constant_operand && found_nonconstant_operand) {
          constant_val = constant_val.sextOrSelf(64);
          *out_val = std::move(nonconstant_val);
          *out_const = std::move(constant_val);
          return true;
        } else {
          return false;
        }
      }
      default: {
        return false;
      }
    }
  } else {
    return false;
  }
}

static bool isAllocatingCall(const CallBase &call) {
  Function* callee = call.getCalledFunction();
  if (!callee) {
    // we assume indirect calls aren't allocating
    return false;
  }
  if (!callee->hasName()) {
    // we assume anonymous functions aren't allocating
    return false;
  }
  StringRef name = callee->getName();
  if (name == "malloc"
    || name == "realloc"
    || name == "calloc"
    || name == "zalloc") {
    return true;
  }
  return false;
}

static Constant* createGlobalConstStr(Module* mod, const char* global_name, const char* str) {
  LLVMContext& ctx = mod->getContext();
  Constant* strConst = ConstantDataArray::getString(ctx, str);
  Constant* strGlobal = mod->getOrInsertGlobal(global_name, strConst->getType());
  cast<GlobalVariable>(strGlobal)->setInitializer(strConst);
  cast<GlobalVariable>(strGlobal)->setLinkage(GlobalValue::PrivateLinkage);
  return strGlobal;
}

static std::string regexSubAll(const Regex &R, const StringRef Repl, const StringRef String) {
  std::string curString = String.str();
  while (R.match(curString)) {
    curString = R.sub(Repl, curString);
  }
  return curString;
}
