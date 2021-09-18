#include "llvm/Transforms/Utils/DMS.h"
#include "llvm/Transforms/Utils/DMS_BoundsInfo.h"
#include "llvm/Transforms/Utils/DMS_Induction.h"
#include "llvm/Transforms/Utils/DMS_PointerStatus.h"

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

#define DEBUG_TYPE "DMS"

const APInt zero = APInt(/* bits = */ 64, /* val = */ 0);
const APInt minusone = APInt(/* bits = */ 64, /* val = */ -1);

template <typename K, typename V, unsigned N> static bool mapsAreEqual(const SmallDenseMap<K, V, N> &A, const SmallDenseMap<K, V, N> &B);
static void describePointerList(const SmallVector<const Value*, 8>& ptrs, std::ostringstream& out, StringRef desc);
static void setInsertPointToAfterInst(IRBuilder<>& Builder, Instruction* inst);
static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep);
static bool isAllocatingCall(const CallBase &call);
static bool shouldCountCallForStatsPurposes(const CallBase &call);
static Constant* createGlobalConstStr(Module* mod, const char* global_name, const char* str);
static std::string regexSubAll(const Regex &R, const StringRef Repl, const StringRef String);

/// Return type for `classifyGEPResult`.
struct GEPResultClassification {
  /// Classification of the result of the given `gep`.
  PointerStatus classification;
  /// Was the total offset of the GEP considered a constant?
  /// (If `override_constant_offset` is non-NULL, this will always be `true`, of
  /// course.)
  bool offset_is_constant;
  /// If `offset_is_constant` is `true`, then this holds the value of the
  /// constant offset.
  /// (If `override_constant_offset` is non-NULL, `classifyGEPResult` will
  /// copy that value to `constant_offset`.)
  APInt constant_offset;
  /// If `constant_offset` is nonzero, do we consider it as zero anyways because
  /// it is a "trustworthy" struct offset?
  bool trustworthy_struct_offset;
};
/// Classify the `PointerStatus` of the result of the given `gep`, assuming that its
/// input pointer is `input_status`.
/// This looks only at the `GetElementPtrInst` itself, and thus does not try to
/// do any loop induction reasoning etc (that is done elsewhere).
/// Think of this as giving the raw/default result for the `gep`.
///
/// `override_constant_offset`: if this is not NULL, then ignore the GEP's indices
/// and classify it as if the offset were the given compile-time constant.
static GEPResultClassification classifyGEPResult(const GetElementPtrInst &gep, const PointerStatus input_status, const DataLayout &DL, const bool trustLLVMStructTypes, const APInt* override_constant_offset);

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
    mark_as(ptr, PointerKind::CLEAN);
  }

  void mark_dirty(const Value* ptr) {
    mark_as(ptr, PointerKind::DIRTY);
  }

  void mark_blemished16(const Value* ptr) {
    mark_as(ptr, PointerKind::BLEMISHED16);
  }

  void mark_blemished32(const Value* ptr) {
    mark_as(ptr, PointerKind::BLEMISHED32);
  }

  void mark_blemished64(const Value* ptr) {
    mark_as(ptr, PointerKind::BLEMISHED64);
  }

  void mark_blemishedconst(const Value* ptr) {
    mark_as(ptr, PointerKind::BLEMISHEDCONST);
  }

  void mark_unknown(const Value* ptr) {
    mark_as(ptr, PointerKind::UNKNOWN);
  }

  /// `dynamic_kind` may be NULL before `pointer_encoding_is_complete` -- in
  /// particular if we're not doing pointer encoding at all.
  /// (If we aren't inserting dynamic instructions for pointer encoding, we
  /// don't have the dynamic Values representing dynamic kinds.)
  /// If/when `pointer_encoding_is_complete`, `dynamic_kind` must not be NULL
  /// in any call to `mark_dynamic`.
  void mark_dynamic(const Value* ptr, Value* dynamic_kind) {
    mark_as(ptr, PointerStatus::dynamic(dynamic_kind));
  }

  // Use this for any `kind` except NOTDEFINEDYET or DYNAMIC
  void mark_as(const Value* ptr, PointerKind kind) {
    // don't explicitly mark anything NOTDEFINEDYET - we reserve
    // "not in the map" to mean NOTDEFINEDYET
    assert(kind != PointerKind::NOTDEFINEDYET);
    // DYNAMIC has to be handled with `mark_dynamic` or the other overload
    // of `mark_as`
    assert(kind != PointerKind::DYNAMIC);
    // insert() does nothing if the key was already in the map.
    // instead, it appears we have to use operator[], which seems to
    // work whether or not `ptr` was already in the map
    map[ptr] = { kind, NULL };
  }

  // Use this for any `kind` except NOTDEFINEDYET
  void mark_as(const Value* ptr, PointerStatus status) {
    // don't explicitly mark anything NOTDEFINEDYET - we reserve
    // "not in the map" to mean NOTDEFINEDYET
    assert(status.kind != PointerKind::NOTDEFINEDYET);
    map[ptr] = status;
  }

  PointerStatus getStatus(const Value* ptr) const {
    auto it = map.find(ptr);
    if (it != map.end()) {
      // found it in the map
      return it->getSecond();
    }
    // if we get here, the pointer wasn't found in the map. Is it a constant pointer?
    if (const Constant* constant = dyn_cast<Constant>(ptr)) {
      if (constant->isNullValue()) {
        // the null pointer can be considered CLEAN
        return PointerStatus::clean();
      } else if (isa<UndefValue>(constant)) {
        // undef values, which includes poison values, can be considered CLEAN
        return PointerStatus::clean();
      } else if (const ConstantExpr* expr = dyn_cast<ConstantExpr>(constant)) {
        // it's a pointer created by a compile-time constant expression
        if (expr->isGEPWithNoNotionalOverIndexing()) {
          // this seems sufficient to consider the pointer clean, based on docs
          // of this method. GEP on a constant pointer, with constant indices,
          // that LLVM thinks are all in-bounds
          return PointerStatus::clean();
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
            return classifyGEPResult(*gepinst, getStatus(gepinst->getPointerOperand()), DL, trustLLVMStructTypes, NULL).classification;
          }
          default: {
            LLVM_DEBUG(dbgs() << "constant expression of unhandled opcode:\n");
            LLVM_DEBUG(expr->dump());
            llvm_unreachable("getting status of constant expression of unhandled opcode");
          }
        }
      } else {
        // a constant, but not null and not a constant expression.
        LLVM_DEBUG(dbgs() << "constant pointer of unhandled kind:\n");
        LLVM_DEBUG(constant->dump());
        llvm_unreachable("getting status of constant pointer of unhandled kind");
      }
    } else {
      // not found in map, and not a constant.
      return PointerStatus::notdefinedyet();
    }
  }

  bool isEqualTo(const PointerStatuses& other) const {
    // since we assert in `mark_as()` that we never explicitly mark anything
    // NOTDEFINEDYET, we can just check that the `map`s contain exactly the same
    // elements mapped to the same things
    return mapsAreEqual(map, other.map);
  }

  std::string describe() const {
    SmallVector<const Value*, 8> clean_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> blem_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> dirty_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> unk_ptrs = SmallVector<const Value*, 8>();
    for (auto& pair : map) {
      const Value* ptr = pair.getFirst();
      if (ptr->hasName() && ptr->getName().startswith("__DMS")) {
        // name starts with __DMS, skip it
        continue;
      }
      switch (pair.getSecond().kind) {
        case PointerKind::CLEAN:
          clean_ptrs.push_back(ptr);
          break;
        case PointerKind::BLEMISHED16:
        case PointerKind::BLEMISHED32:
        case PointerKind::BLEMISHED64:
        case PointerKind::BLEMISHEDCONST:
          blem_ptrs.push_back(ptr);
          break;
        case PointerKind::DIRTY:
          dirty_ptrs.push_back(ptr);
          break;
        case PointerKind::UNKNOWN:
        case PointerKind::DYNAMIC:
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
  /// use the `PointerStatus` `merge` function to combine the two results.
  /// Recall that any pointer not appearing in the `map` is considered NOTDEFINEDYET.
  ///
  /// `insertion_pt`: If we need to insert new dynamic instructions to handle
  /// a dynamic merge, insert them before this Instruction.
  /// We will only potentially need to do this if at least one of the statuses
  /// is DYNAMIC with a non-null `dynamic_kind`. If none of the statuses is
  /// DYNAMIC with a non-null `dynamic_kind`, then this parameter is ignored
  /// (and may be NULL).
  static PointerStatuses merge(const PointerStatuses& a, const PointerStatuses& b, Instruction* insertion_pt) {
    assert(a.DL == b.DL);
    assert(a.trustLLVMStructTypes == b.trustLLVMStructTypes);
    PointerStatuses merged(a.DL, a.trustLLVMStructTypes);
    for (const auto& pair : a.map) {
      const Value* ptr = pair.getFirst();
      const PointerStatus status_in_a = pair.getSecond();
      const auto& it = b.map.find(ptr);
      PointerStatus status_in_b = (it == b.map.end()) ?
        // implicitly NOTDEFINEDYET in b
        PointerStatus::notdefinedyet() :
        // defined in b, get the status
        it->getSecond();
      merged.mark_as(ptr, PointerStatus::merge(status_in_a, status_in_b, insertion_pt));
    }
    // at this point we've handled all the pointers which were defined in a.
    // what's left is the pointers which were defined in b and NOTDEFINEDYET in a
    for (const auto& pair : b.map) {
      const Value* ptr = pair.getFirst();
      const PointerStatus status_in_b = pair.getSecond();
      const auto& it = a.map.find(ptr);
      if (it == a.map.end()) {
        // implicitly NOTDEFINEDYET in a
        merged.mark_as(ptr, PointerStatus::merge(PointerStatus::notdefinedyet(), status_in_b, insertion_pt));
      }
    }
    return merged;
  }

private:
  const DataLayout &DL;
  const bool trustLLVMStructTypes;
  /// Maps a pointer to its status.
  /// Pointers not appearing in this map are considered NOTDEFINEDYET.
  /// As a corollary, hopefully all pointers which are currently live do appear
  /// in this map.
  SmallDenseMap<const Value*, PointerStatus, 8> map;
};

template<typename K, typename V, unsigned N>
static bool mapsAreEqual(const SmallDenseMap<K, V, N> &A, const SmallDenseMap<K, V, N> &B) {
  // first: maps of different sizes can never be equal
  if (A.size() != B.size()) return false;
  // now check that all keys in A are also in B, and map to the same things
  for (const auto &pair : A) {
    const auto& it = B.find(pair.getFirst());
    if (it == B.end()) {
      // key wasn't in B
      return false;
    }
    if (it->getSecond() != pair.getSecond()) {
      // maps disagree on what this key maps to
      return false;
    }
  }
  // we don't need the reverse check (all keys in B are also in A) because we
  // already checked that A and B have the same number of keys, and all keys in
  // A are also in B
  return true;
}

class DMSAnalysis final {
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
  DMSAnalysis(Function &F, FunctionAnalysisManager &FAM, bool trustLLVMStructTypes, PointerKind inttoptr_kind, bool do_pointer_encoding)
    : F(F), DL(F.getParent()->getDataLayout()),
      loopinfo(FAM.getResult<LoopAnalysis>(F)), pdtree(FAM.getResult<PostDominatorTreeAnalysis>(F)),
      trustLLVMStructTypes(trustLLVMStructTypes), inttoptr_kind(inttoptr_kind),
      RPOT(ReversePostOrderTraversal<BasicBlock *>(&F.getEntryBlock())),
      do_pointer_encoding(do_pointer_encoding),
      pointer_encoding_is_complete(false) {
    initialize_block_states();
  }
  ~DMSAnalysis() {
    // clean up the `PerBlockState`s which were created with `new`
    for (auto& pair : block_states) {
      delete pair.getSecond();
    }
  }

  struct StaticCounts {
    unsigned clean;
    unsigned blemished16;
    unsigned blemished32;
    unsigned blemished64;
    unsigned blemishedconst;
    unsigned dirty;
    unsigned unknown; // the static "unknown" category includes all pointers with dynamic status

    StaticCounts operator+(const StaticCounts& other) const {
      return StaticCounts {
        clean + other.clean,
        blemished16 + other.blemished16,
        blemished32 + other.blemished32,
        blemished64 + other.blemished64,
        blemishedconst + other.blemishedconst,
        dirty + other.dirty,
        unknown + other.unknown,
      };
    }

    StaticCounts& operator+=(const StaticCounts& other) {
      clean += other.clean;
      blemished16 += other.blemished16;
      blemished32 += other.blemished32;
      blemished64 += other.blemished64;
      blemishedconst += other.blemishedconst;
      dirty += other.dirty;
      unknown += other.unknown;
      return *this;
    }
  };

  /// This struct holds the STATIC results of the analysis
  struct StaticResults {
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

    StaticResults operator+(const StaticResults& other) const {
      return StaticResults {
        load_addrs + other.load_addrs,
        store_addrs + other.store_addrs,
        store_vals + other.store_vals,
        passed_ptrs + other.passed_ptrs,
        returned_ptrs + other.returned_ptrs,
        pointer_arith_const + other.pointer_arith_const,
        inttoptrs + other.inttoptrs,
      };
    }

    StaticResults& operator+=(const StaticResults& other) {
      load_addrs += other.load_addrs;
      store_addrs += other.store_addrs;
      store_vals += other.store_vals;
      passed_ptrs += other.passed_ptrs;
      returned_ptrs += other.returned_ptrs;
      pointer_arith_const += other.pointer_arith_const;
      inttoptrs += other.inttoptrs;
      return *this;
    }
  };

  /// Holds the IR global variables containing dynamic counts
  struct DynamicCounts {
    Constant* clean;
    Constant* blemished16;
    Constant* blemished32;
    Constant* blemished64;
    Constant* blemishedconst;
    Constant* dirty;
    Constant* unknown;
  };

  /// This struct holds the IR global variables representing the DYNAMIC results
  /// of the analysis
  struct DynamicResults {
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
  };

  /// Runs the analysis and returns the `StaticResults`
  StaticResults run() {
    IterationResult res;
    res.changed = true;

    while (res.changed) {
      res = doIteration(NULL, false);
    }

    return res.static_results;
  }

  /// Where to print results dynamically (at runtime)
  typedef enum DynamicPrintType {
    /// Print to stdout
    STDOUT,
    /// Print to a file in ./dms_dynamic_counts
    TOFILE,
  } DynamicPrintType;

  /// Instruments the code for dynamic counts.
  /// You _must_ run() the analysis first -- instrument() assumes that the
  /// analysis is complete.
  void instrument(DynamicPrintType print_type) {
    DynamicResults results = initializeDynamicResults();
    IterationResult res = doIteration(NULL, false);
    assert(!res.changed && "Don't run instrument() until analysis has reached fixpoint");
    doIteration(&results, false);
    addDynamicResultsPrint(results, print_type);
  }

  /// Adds SW spatial safety checks where necessary.
  /// You _must_ run() the analysis first -- addSpatialSafetySWChecks() assumes
  /// that the analysis is complete.
  void addSpatialSafetySWChecks() {
    IterationResult res = doIteration(NULL, true);
    assert(!res.changed && "Don't run addSpatialSafetyChecks() until analysis has reached fixpoint");
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
  const DataLayout &DL;
  const LoopInfo& loopinfo;
  const PostDominatorTree& pdtree;
  const bool trustLLVMStructTypes;
  const PointerKind inttoptr_kind;

  /// we use "reverse post order" in an attempt to process block predecessors
  /// before the blocks themselves. (Of course, this isn't possible to do
  /// perfectly, because of loops.)
  /// We also store this as a class member because constructing it is expensive,
  /// according to the docs in PostOrderIterator.h. We don't want to construct it
  /// each time it's needed in `doIteration()`.
  const ReversePostOrderTraversal<BasicBlock*> RPOT;

  /// True if the `DMSAnalysis` should modify the in-memory representation of
  /// pointers, using bits 48 and 49 to indicate the DynamicPointerKind.
  /// This informs dynamic counts.
  const bool do_pointer_encoding;

  /// True if we've already inserted the dynamic instructions that encode/decode
  /// pointers stored in memory.
  bool pointer_encoding_is_complete;

  /// This holds the per-block state for the analysis
  class PerBlockState final {
  public:
    PerBlockState(const DataLayout &DL, const bool trustLLVMStructTypes)
      : ptrs_beg(PointerStatuses(DL, trustLLVMStructTypes)),
        ptrs_end(PointerStatuses(DL, trustLLVMStructTypes)),
        static_results(StaticResults { 0 }) {}

    /// The status of all pointers at the _beginning_ of the block.
    PointerStatuses ptrs_beg;
    /// The status of all pointers at the _end_ of the block.
    PointerStatuses ptrs_end;
    /// The `StaticResults` which we got last time we analyzed this block.
    DMSAnalysis::StaticResults static_results;
  };

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
        bounds_info[&arg] = BoundsInfo::unknown();
      }
    }

    // Mark pointers to global variables (and other global values, e.g.,
    // functions and IFuncs) as CLEAN in the function's entry block.
    // (If the global variable itself is a pointer, it's still implicitly
    // NOTDEFINEDYET.)
    for (const GlobalValue& gv : F.getParent()->global_values()) {
      assert(gv.getType()->isPointerTy());
      entry_block_pbs->ptrs_beg.mark_clean(&gv);
      // we know the bounds of the global allocation statically
      Type* globalType = gv.getValueType();
      if (globalType->isSized()) {
        auto allocationSize = DL.getTypeStoreSize(globalType).getFixedSize();
        bounds_info[&gv] = BoundsInfo::static_bounds(
          zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
        );
      } else {
        bounds_info[&gv] = BoundsInfo::unknown();
      }
    }
  }

  /// If the `DMSAnalysis` has `do_pointer_encoding`, this maps loaded value
  /// (type `LoadInst`) to its `PointerStatus` at the program point right after
  /// it is loaded (which will be DYNAMIC and contain the loaded value's
  /// `dynamic_kind`). This mapping can't change from iteration to iteration.
  DenseMap<const LoadInst*, PointerStatus> loaded_val_statuses;

  /// This maps pointer-typed PHI nodes to the corresponding PHI of the
  /// `dynamic_kind`s, if one has been created.
  DenseMap<const PHINode*, PHINode*> dynamic_phi_to_status_phi;

  /// For the IntToPtrs in this map, don't count them for stats, and lock the
  /// result's PointerStatus and BoundsInfo to be the same status/boundsinfo as
  /// that of the corresponding Value.
  /// This is used for the IntToPtrs which we insert ourselves as part of
  /// pointer encoding/decoding.
  DenseMap<const IntToPtrInst*, const Value*> inttoptr_status_and_bounds_overrides;

  /// For the Instructions in this set, don't count them for stats or do any
  /// other operations with them. They aren't from the original program, they
  /// were inserted by us to facilitate tracking bounds. The results of these
  /// instructions will be used only in bounds checks, if ever; if they are
  /// pointers, they will never be dereferenced.
  ///
  /// We don't necessarily add all the instructions we insert for bounds
  /// purposes to this set. But, the intent is that we at minimum add all the
  /// pointer-producing instructions inserted for bounds purposes.
  DenseSet<const Instruction*> bounds_insts;

  /// Maps a pointer to its bounds info, if we know anything about its bounds
  /// info.
  /// For pointers not appearing in this map, we don't know anything about their
  /// bounds.
  DenseMap<const Value*, BoundsInfo> bounds_info;

  /// Return value for `doIteration`.
  struct IterationResult {
    /// This indicates if any change was made to the pointer statuses in _any_
    /// block.  (If so, we should do another iteration.)
    bool changed;
    /// `StaticResults` for this function
    StaticResults static_results;
  };

  /// `dynamic_results`: if not NULL, then insert instrumentation to collect
  /// dynamic counts in this `DynamicResults` object.
  /// Caller must only pass a non-NULL value for this after the analysis has
  /// reached a fixpoint.
  ///
  /// `add_spatial_sw_checks`: if `true`, then insert SW spatial safety checks
  /// where necessary.
  /// Caller must only set this to `true` after the analysis has reached a
  /// fixpoint.
  /// Currently this assumes that dereferencing BLEMISHED16 pointers does not
  /// need a SW check, but all other BLEMISHED pointers need checks.
  IterationResult doIteration(DynamicResults* dynamic_results, const bool add_spatial_sw_checks) {
    StaticResults static_results = { 0 };
    bool changed = false;

    LLVM_DEBUG(dbgs() << "DMS: starting an iteration through function " << F.getName() << "\n");

    for (BasicBlock* block : RPOT) {
      AnalyzeBlockResult res = analyze_block(*block, dynamic_results, add_spatial_sw_checks);
      changed |= res.end_of_block_statuses_changed;
      static_results += res.static_results;
    }

    if (do_pointer_encoding) pointer_encoding_is_complete = true;

    return IterationResult { changed, static_results };
  }

  /// Compute the `PointerStatuses` for the top of the given block, based on the
  /// current `PointerStatuses` at the end of the block's predecessors.
  ///
  /// Caller must not call this on a block with no predecessors (e.g., the entry
  /// block).
  PointerStatuses computeTopOfBlockPointerStatuses(BasicBlock &block) {
    assert(block.hasNPredecessorsOrMore(1));
    // if any variable is clean at the end of all of this block's predecessors,
    // then it is also clean at the beginning of this block
    auto preds = pred_begin(&block);
    const BasicBlock* firstPred = *preds;
    const PerBlockState* firstPred_pbs = block_states[firstPred];
    // we start with all of the ptr_statuses at the end of our first predecessor,
    // then merge with the ptr_statuses at the end of our other predecessors
    PointerStatuses ptr_statuses = PointerStatuses(firstPred_pbs->ptrs_end);
    DEBUG_WITH_TYPE("DMS-block-stats", dbgs() << "DMS:   first predecessor has " << ptr_statuses.describe() << " at end\n");
    for (auto it = ++preds, end = pred_end(&block); it != end; ++it) {
      const BasicBlock* otherPred = *it;
      const PerBlockState* otherPred_pbs = block_states[otherPred];
      DEBUG_WITH_TYPE("DMS-block-stats", dbgs() << "DMS:   next predecessor has " << otherPred_pbs->ptrs_end.describe() << " at end\n");
      ptr_statuses = PointerStatuses::merge(std::move(ptr_statuses), otherPred_pbs->ptrs_end, &block.front());
    }
    return ptr_statuses;
  }

  /// Return value for `analyze_block`.
  struct AnalyzeBlockResult {
    /// This indicates if any change was made to the pointer statuses at the
    /// _end_ of the block.  (If so, subsequent blocks may need to be
    /// reanalyzed.)
    bool end_of_block_statuses_changed;
    /// `StaticResults` for this block
    StaticResults static_results;
  };

  /// `dynamic_results`: if not NULL, then insert instrumentation to collect
  /// dynamic counts in this `DynamicResults` object.
  /// Caller must only pass a non-NULL value for this after the analysis has
  /// reached a fixpoint.
  ///
  /// `add_spatial_sw_checks`: if `true`, then insert SW spatial safety checks
  /// where necessary.
  /// Caller must only set this to `true` after the analysis has reached a
  /// fixpoint.
  /// Currently this assumes that dereferencing BLEMISHED16 pointers does not
  /// need a SW check, but all other BLEMISHED pointers need checks.
  AnalyzeBlockResult analyze_block(
    BasicBlock &block,
    DynamicResults* dynamic_results,
    const bool add_spatial_sw_checks
  ) {
    PerBlockState* pbs = block_states[&block];

    LLVM_DEBUG(
      StringRef blockname;
      if (block.hasName()) {
        blockname = block.getName();
      } else {
        blockname = "<anonymous>";
      }
      dbgs() << "DMS: analyzing block " << blockname << "\n";
    );
    DEBUG_WITH_TYPE("DMS-block-previous-state", dbgs() << "DMS:   this block previously had " << pbs->ptrs_beg.describe() << " at beginning and " << pbs->ptrs_end.describe() << " at end\n");

    bool isEntryBlock = block.hasNPredecessors(0);  // technically a dead block could also have 0 predecessors, but we don't care what this analysis does with dead blocks. (If you run this pass after optimizations there shouldn't be dead blocks anyway.)

    // The current pointer statuses. As we go through the block, this gets
    // updated; its state at the end of the block will become `pbs->ptrs_end`.
    PointerStatuses ptr_statuses = isEntryBlock ?
      // for the entry block, we already correctly initialized the top-of-block
      // pointer statuses, so just retrieve those and return them
      pbs->ptrs_beg :
      // for all other blocks, compute the top-of-block pointer statuses based
      // on the block's predecessors
      computeTopOfBlockPointerStatuses(block);
    DEBUG_WITH_TYPE("DMS-block-stats", dbgs() << "DMS:   at beginning of block, we have " << ptr_statuses.describe() << "\n");

    if (!isEntryBlock) {
      // Let's check if that's any different from what we had last time
      if (ptr_statuses.isEqualTo(pbs->ptrs_beg)) {
        // no change. Unless we're adding dynamic instrumentation, there's no
        // need to actually analyze this block. Just return the `StaticResults`
        // we got last time.
        LLVM_DEBUG(dbgs() << "DMS:   top-of-block statuses haven't changed\n");
        if (!dynamic_results && !add_spatial_sw_checks) return AnalyzeBlockResult { false, pbs->static_results };
        // (you could be concerned that this check could pass on the first
        // iteration if the top-of-block statuses happen to be equal to the
        // initial state of a `PointerStatuses`; and then we wouldn't ever
        // analyze the block, whoops. but this is actually impossible:
        // an initial `PointerStatuses` has no pointers, but every block has at
        // least one pointer status at top-of-block: namely, the pointer to the
        // current function, which will be in the `PointerStatuses` as CLEAN)
      } else {
        // save the top-of-block ptr_statuses so we can do the above check on
        // the next iteration
        pbs->ptrs_beg = ptr_statuses;
      }
    }

    StaticResults static_results = { 0 };

    // Count an operation occurring in the given `category` with the given `kind`.
    // `ip` is the insert point: if dynamic instructions must be inserted (to do
    // dynamic counting), they will be inserted before the Instruction `ip`
    #define COUNT_OP_KIND(category, kind, ip) \
      static_results.category.kind++; \
      if (dynamic_results) { \
        incrementGlobalCounter(dynamic_results->category.kind, (ip)); \
      }

    // same as COUNT_OP_KIND, but for a dynamic kind.
    // This is counted as UNKNOWN statically, but has an actual kind
    // dynamically.
    #define COUNT_OP_DYN(category, status, ip) \
      static_results.category.unknown++; \
      if (dynamic_results) { \
        incrementGlobalCounterForDynKind(dynamic_results->category, (status), (ip)); \
      }

    // Count an operation occurring in the given `category` with the given `status`.
    // `ip` is the insert point: if dynamic instructions must be inserted (to do
    // dynamic counting), they will be inserted before the Instruction `ip`
    #define COUNT_OP_AS_STATUS(category, status, ip, doing_what) \
      PointerStatus the_status = (status); /* in case (status) is an expensive-to-compute expression, compute it once here */ \
      switch (the_status.kind) { \
        case PointerKind::CLEAN: COUNT_OP_KIND(category, clean, ip) break; \
        case PointerKind::BLEMISHED16: COUNT_OP_KIND(category, blemished16, ip) break; \
        case PointerKind::BLEMISHED32: COUNT_OP_KIND(category, blemished32, ip) break; \
        case PointerKind::BLEMISHED64: COUNT_OP_KIND(category, blemished64, ip) break; \
        case PointerKind::BLEMISHEDCONST: COUNT_OP_KIND(category, blemishedconst, ip) break; \
        case PointerKind::DIRTY: COUNT_OP_KIND(category, dirty, ip) break; \
        case PointerKind::UNKNOWN: COUNT_OP_KIND(category, unknown, ip) break; \
        case PointerKind::DYNAMIC: COUNT_OP_DYN(category, the_status, ip) break; \
        case PointerKind::NOTDEFINEDYET: llvm_unreachable(doing_what " with no status"); break; \
        default: llvm_unreachable("PointerKind case not handled"); \
      }

    // now: process each instruction
    // we only need to worry about pointer dereferences, and instructions which
    // produce pointers
    // (and of course we want to statically count a few other kinds of events)
    for (Instruction &inst : block) {
      if (bounds_insts.count(&inst)) {
        // see notes on bounds_insts
        continue;
      }
      switch (inst.getOpcode()) {
        case Instruction::Store: {
          StoreInst& store = cast<StoreInst>(inst);
          // first, if we're storing a pointer we have some extra work to do.
          Value* storedVal = store.getValueOperand();
          if (storedVal->getType()->isPointerTy()) {
            // we count the stored pointer for stats purposes
            PointerStatus storedVal_status = ptr_statuses.getStatus(storedVal);
            COUNT_OP_AS_STATUS(store_vals, storedVal_status, &inst, "Storing a pointer");
            if (pointer_encoding_is_complete) {
              // update the mask if necessary, based on our knowledge of the
              // status of `storedVal`. See below case, for
              // `do_pointer_encoding`.

              // After `pointer_encoding_is_complete`, all stored pointers
              // should be results of `IntToPtr` instructions
              IntToPtrInst* storedVal_inst = cast<IntToPtrInst>(storedVal);

              // Compute the updated mask, i.e. the one that we should be
              // using based on our most up-to-date knowledge of status of
              // `storedVal`
              IRBuilder<> Builder(storedVal_inst);
              Value* new_mask = storedVal_status.to_dynamic_kind_mask(Builder);

              // `maskedVal` is the integer resulting from applying the mask
              Value* maskedVal = storedVal_inst->getOperand(0);
              // maskedVal should also be the result of some kind of `Instruction`
              Instruction* maskedVal_inst = cast<Instruction>(maskedVal);
              if (maskedVal_inst->getOpcode() == Instruction::Or) {
                // maskedVal is the result of a literal `Or` instruction
                // applying a mask. The mask itself is the second operand.
                Value* mask = maskedVal_inst->getOperand(1);
                if (mask == new_mask) {
                  // nothing to do
                } else if (ConstantInt* mask_const = dyn_cast<ConstantInt>(mask)) {
                  if (ConstantInt* new_mask_const = dyn_cast<ConstantInt>(new_mask)) {
                    if (mask_const->getValue() == new_mask_const->getValue()) {
                      // nothing to do
                    } else {
                      // both masks are constants, but different values.
                      // change the mask to the new mask. Nothing else needs to change
                      maskedVal_inst->setOperand(1, new_mask);
                    }
                  } else {
                    // old mask is a constant, new one is not.
                    // change the mask to the new mask. Nothing else needs to change
                    maskedVal_inst->setOperand(1, new_mask);
                  }
                } else {
                  // old mask is a non-constant, new one is some other constant or non-constant.
                  // change the mask to the new mask. Nothing else needs to change
                  maskedVal_inst->setOperand(1, new_mask);
                }
              } else {
                // maskedVal is not the result of a literal `Or` instruction.
                // This must mean that the current mask was constant 0.
                // If the new mask is as well, then we're all good
                Constant* new_mask_const = dyn_cast<Constant>(new_mask);
                if (new_mask_const && new_mask_const->isNullValue()) {
                  // nothing to do
                } else {
                  // old mask was constant 0, new one is not. We need to insert
                  // a new `Or` instruction to apply the new mask
                  Value* new_maskedVal = Builder.CreateOr(maskedVal, new_mask);
                  // update storedVal_inst to use this new masked value as its input
                  storedVal_inst->setOperand(0, new_maskedVal);
                }
              }
            } else if (do_pointer_encoding) {
              // modify the store instruction to store the encoded pointer
              // instead.
              // Specifically, when we store the pointer to memory, we use bits
              // 48-49 to indicate its PointerKind, interpreted per the
              // DynamicPointerKind enum.
              // When we later load this pointer from memory, we'll check bits
              // 48-49 to learn the pointer type, then clear them so the pointer
              // is valid for use.
              // (We assume all pointers are userspace pointers, so 48-49 should
              // be 0 for valid pointers.)
              LLVM_DEBUG(dbgs() << "DMS:   encoding a stored pointer\n");
              // create `new_storedVal` which has the appropriate bits set
              IRBuilder<> Builder(&store);
              Value* mask = storedVal_status.to_dynamic_kind_mask(Builder);
              Value* storedVal_as_int = Builder.CreatePtrToInt(storedVal, Builder.getInt64Ty());
              Value* new_storedVal = Builder.CreateOr(storedVal_as_int, mask);
              Value* new_storedVal_as_ptr = Builder.CreateIntToPtr(new_storedVal, storedVal->getType());
              // store the new (encoded) value instead of the old one
              store.setOperand(0, new_storedVal_as_ptr);
              // and also mark the status of the new (encoded) value -- it's the
              // same as the status of the original (unencoded) value
              ptr_statuses.mark_as(new_storedVal_as_ptr, storedVal_status);
              // create a status override for the new `IntToPtr`, so this
              // relationship is preserved even in future passes. It will always
              // have the same status as `storedVal`.
              IntToPtrInst* inttoptr = cast<IntToPtrInst>(new_storedVal_as_ptr);
              inttoptr_status_and_bounds_overrides[inttoptr] = storedVal;
            }
          }
          // next count the address
          const Value* addr = store.getPointerOperand();
          COUNT_OP_AS_STATUS(store_addrs, ptr_statuses.getStatus(addr), &inst, "Storing to pointer");
          // insert a bounds check before the store, if necessary
          if (add_spatial_sw_checks) {
            IRBuilder<> BeforeStore(&store);
            maybeAddSpatialSWCheck(addr, ptr_statuses.getStatus(addr), BeforeStore);
          }
          // now, the pointer used as an address becomes clean
          ptr_statuses.mark_clean(addr);
          break;
        }
        case Instruction::Load: {
          LoadInst& load = cast<LoadInst>(inst);
          const Value* ptr = load.getPointerOperand();
          // first count this for static stats
          COUNT_OP_AS_STATUS(load_addrs, ptr_statuses.getStatus(ptr), &inst, "Loading from pointer");
          // insert a bounds check before the load, if necessary
          if (add_spatial_sw_checks) {
            IRBuilder<> BeforeLoad(&load);
            maybeAddSpatialSWCheck(ptr, ptr_statuses.getStatus(ptr), BeforeLoad);
          }
          // now, the pointer becomes clean
          ptr_statuses.mark_clean(ptr);

          if (load.getType()->isPointerTy()) {
            // in this case, we loaded a pointer from memory, so we
            // only know its status and bounds dynamically, not statically.
            // See notes above on the Store case.
            if (pointer_encoding_is_complete) {
              // get the status from `loaded_val_statuses`; see notes there
              PointerStatus status = loaded_val_statuses[&load];
              ptr_statuses.mark_as(&load, status);
              // bounds info remains valid from iteration to iteration (our
              // fixpoint won't change the bounds info here), so we don't
              // need to change anything now. we computed the bounds info
              // when we did the pointer encoding.
              assert(bounds_info.count(&load) > 0);
            } else if (do_pointer_encoding) {
              // insert the instructions to interpret the encoded pointer
              IRBuilder<> AfterLoad(&block);
              setInsertPointToAfterInst(AfterLoad, &load);
              Value* val_as_int = AfterLoad.CreatePtrToInt(&load, AfterLoad.getInt64Ty());
              Value* dynamic_kind = AfterLoad.CreateAnd(val_as_int, DynamicKindMasks::dynamic_kind_mask);
              Value* new_val = AfterLoad.CreateAnd(val_as_int, ~DynamicKindMasks::dynamic_kind_mask);
              Value* new_val_as_ptr = AfterLoad.CreateIntToPtr(new_val, load.getType());
              // replace all uses of `load` with the modified loaded ptr, except
              // of course the use which we just inserted (which generates
              // `val_as_int`), and any uses in `bounds_insts`
              load.replaceUsesWithIf(
                new_val_as_ptr,
                [val_as_int](Use &U){ return U.getUser() != val_as_int; }
              );
              PointerStatus status = PointerStatus::dynamic(dynamic_kind);
              ptr_statuses.mark_as(&load, status);
              ptr_statuses.mark_as(new_val_as_ptr, status);
              // store this mapping in `loaded_val_statuses`; see notes there
              loaded_val_statuses[&load] = status;
              // create a status and bounds override for the `IntToPtr` which we
              // just inserted: it should always have the same dynamic status
              // and bounds as the loaded value. (The loaded value itself will
              // always have the dynamic `status` we just created, thanks to
              // `loaded_val_statuses`.)
              IntToPtrInst* new_val_inttoptr = cast<IntToPtrInst>(new_val_as_ptr);
              inttoptr_status_and_bounds_overrides[new_val_inttoptr] = &load;
              // compute the bounds of the loaded pointer dynamically. this
              // requires the unencoded pointer value, ie `new_val_as_ptr`.
              // TODO: Instead of loading bounds info right when we load the
              // pointer, we could/should wait until it is needed for a SW
              // bounds check. I'm envisioning some type of laziness solution
              // inside BoundsInfo.
              BoundsInfo::DynamicBoundsInfo loadedInfo = load_dynamic_boundsinfo(new_val_as_ptr, AfterLoad, bounds_insts);
              bounds_info[&load] = BoundsInfo(std::move(loadedInfo));
            } else {
              // when not `do_pointer_encoding`, we're allowed to pass NULL
              // here. See notes on `mark_dynamic`
              ptr_statuses.mark_dynamic(&load, NULL);
              // bounds info remains valid from iteration to iteration (our
              // fixpoint won't change the bounds info here), so we only need to
              // insert the instructions computing it the first time.
              // TODO: Instead of loading bounds info right when we load the
              // pointer, we could/should wait until it is needed for a SW
              // bounds check. I'm envisioning some type of laziness solution
              // inside BoundsInfo.
              if (bounds_info.count(&load) == 0) {
                IRBuilder<> AfterLoad(&block);
                setInsertPointToAfterInst(AfterLoad, &load);
                BoundsInfo::DynamicBoundsInfo loadedInfo = load_dynamic_boundsinfo(&load, AfterLoad, bounds_insts);
                bounds_info[&load] = BoundsInfo(std::move(loadedInfo));
              }
            }
          }
          break;
        }
        case Instruction::Alloca: {
          // result of an alloca is a clean pointer
          ptr_statuses.mark_clean(&inst);
          // we know the bounds of the allocation statically
          PointerType* resultType = cast<PointerType>(inst.getType());
          auto allocationSize = DL.getTypeStoreSize(resultType->getElementType()).getFixedSize();
          bounds_info[&inst] = BoundsInfo::static_bounds(
            zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
          );
          break;
        }
        case Instruction::GetElementPtr: {
          GetElementPtrInst& gep = cast<GetElementPtrInst>(inst);
          Value* input_ptr = gep.getPointerOperand();
          PointerStatus input_status = ptr_statuses.getStatus(input_ptr);
          InductionPatternResult ipr = isOffsetAnInductionPattern(gep, DL, loopinfo, pdtree);
          if (ipr.is_induction_pattern && ipr.induction_offset.isNonNegative() && ipr.initial_offset.isNonNegative())
          {
            if (ipr.initial_offset.sge(ipr.induction_offset)) {
              ipr.induction_offset = std::move(ipr.initial_offset);
            }
            LLVM_DEBUG(dbgs() << "DMS:   found an induction GEP with offset effectively constant " << ipr.induction_offset << "\n");
          } else {
            // we don't consider it an induction pattern if it had negative initial and/or induction offsets
            ipr.is_induction_pattern = false;
          }
          GEPResultClassification grc = classifyGEPResult(gep, input_status, DL, trustLLVMStructTypes, ipr.is_induction_pattern ? &ipr.induction_offset : NULL);
          ptr_statuses.mark_as(&gep, grc.classification);
          // if we added a nonzero constant to a pointer, count that for stats purposes
          if (grc.offset_is_constant && !grc.trustworthy_struct_offset && grc.constant_offset != zero) {
            COUNT_OP_AS_STATUS(pointer_arith_const, input_status, &gep, "GEP on a pointer");
          }
          // propagate the input pointer's bounds to the new pointer. We let
          // the new pointer still have access to the whole allocation
          const BoundsInfo& binfo = bounds_info.lookup(input_ptr);
          switch (binfo.get_kind()) {
            case BoundsInfo::UNKNOWN:
              bounds_info[&gep] = binfo;
              break;
            case BoundsInfo::INFINITE:
              bounds_info[&gep] = binfo;
              break;
            case BoundsInfo::STATIC: {
              const BoundsInfo::StaticBoundsInfo* static_info = binfo.static_info();
              if (grc.offset_is_constant) {
                bounds_info[&gep] = BoundsInfo::static_bounds(
                  static_info->low_offset + grc.constant_offset,
                  static_info->high_offset - grc.constant_offset
                );
              } else {
                // bounds of the new pointer aren't known statically
                // and actually, we don't care what the dynamic GEP offset is:
                // it doesn't change the `base` and `max` of the allocation
                const BoundsInfo::StaticBoundsInfo* input_static_info = binfo.static_info();
                // `base` is `input_ptr` minus the input pointer's low_offset
                const BoundsInfo::PointerWithOffset base = BoundsInfo::PointerWithOffset(input_ptr, -input_static_info->low_offset);
                // `max` is `input_ptr` plus the input pointer's high_offset
                const BoundsInfo::PointerWithOffset max = BoundsInfo::PointerWithOffset(input_ptr, input_static_info->high_offset);
                bounds_info[&gep] = BoundsInfo::dynamic_bounds(base, max);
              }
              break;
            }
            case BoundsInfo::DYNAMIC:
            case BoundsInfo::DYNAMIC_MERGED:
            {
              // regardless of the GEP offset, the `base` and `max` don't change
              bounds_info[&gep] = binfo;
              break;
            }
            default:
              llvm_unreachable("Missing BoundsInfo.kind case");
          }
          break;
        }
        case Instruction::BitCast: {
          const BitCastInst& bitcast = cast<BitCastInst>(inst);
          if (bitcast.getType()->isPointerTy()) {
            const Value* input_ptr = bitcast.getOperand(0);
            ptr_statuses.mark_as(&bitcast, ptr_statuses.getStatus(input_ptr));
            // also propagate bounds info
            const BoundsInfo& binfo = bounds_info.lookup(input_ptr);
            bounds_info[&bitcast] = binfo;
          }
          break;
        }
        case Instruction::AddrSpaceCast: {
          const Value* input_ptr = inst.getOperand(0);
          ptr_statuses.mark_as(&inst, ptr_statuses.getStatus(input_ptr));
          // also propagate bounds info
          const BoundsInfo& binfo = bounds_info.lookup(input_ptr);
          bounds_info[&inst] = binfo;
          break;
        }
        case Instruction::Select: {
          SelectInst& select = cast<SelectInst>(inst);
          if (select.getType()->isPointerTy()) {
            // output is clean if both inputs are clean; etc
            const Value* true_input = select.getTrueValue();
            const Value* false_input = select.getFalseValue();
            const PointerStatus true_status = ptr_statuses.getStatus(true_input);
            const PointerStatus false_status = ptr_statuses.getStatus(false_input);
            ptr_statuses.mark_as(&select, PointerStatus::merge(true_status, false_status, &inst));
            // also propagate bounds info
            const BoundsInfo& binfo1 = bounds_info.lookup(true_input);
            const BoundsInfo& binfo2 = bounds_info.lookup(false_input);
            const BoundsInfo& prev_iteration_binfo = bounds_info.lookup(&select);
            if (
              prev_iteration_binfo.get_kind() == BoundsInfo::DYNAMIC_MERGED
              && prev_iteration_binfo.merge_inputs.size() == 2
              && *prev_iteration_binfo.merge_inputs[0] == binfo1
              && *prev_iteration_binfo.merge_inputs[1] == binfo2
            ) {
              // no need to update
            } else {
              // the merge may need to insert instructions that use the final
              // value of the select, so we need a builder pointing after the
              // select
              IRBuilder<> AfterSelect(&block);
              setInsertPointToAfterInst(AfterSelect, &select);
              bounds_info[&select] = BoundsInfo::merge(binfo1, binfo2, &select, AfterSelect, bounds_insts);
            }
          }
          break;
        }
        case Instruction::PHI: {
          PHINode& phi = cast<PHINode>(inst);
          IRBuilder<> Builder(&inst);
          if (phi.getType()->isPointerTy()) {
            SmallVector<std::pair<PointerStatus, BasicBlock*>, 4> incoming_statuses;
            SmallVector<std::pair<BoundsInfo*, BasicBlock*>, 4> incoming_binfos;
            for (const Use& use : phi.incoming_values()) {
              BasicBlock* bb = phi.getIncomingBlock(use);
              auto& ptr_statuses_end_of_bb = block_states[bb]->ptrs_end;
              const Value* value = use.get();
              incoming_statuses.push_back(std::make_pair(
                ptr_statuses_end_of_bb.getStatus(value),
                bb
              ));
              incoming_binfos.push_back(std::make_pair(
                new BoundsInfo(bounds_info.lookup(value)),
                bb
              ));
            }
            // check for an entry in dynamic_phi_to_status_phi
            auto it = dynamic_phi_to_status_phi.find(&phi);
            if (it != dynamic_phi_to_status_phi.end()) {
              // there's a previously created PHI of the pointer statuses.
              // We need to check if it's still valid -- or if it needs updating
              // because some incoming pointer statuses have changed.
              PHINode* status_phi = it->getSecond();
              assert(incoming_statuses.size() == status_phi->getNumIncomingValues());
              for (auto& pair : incoming_statuses) {
                const PointerStatus& incoming_status = pair.first;
                const BasicBlock* incoming_bb = pair.second;
                const Value* old_incoming_kind = status_phi->getIncomingValueForBlock(incoming_bb);
                assert(old_incoming_kind && "status_phi should have the same incoming blocks as the main phi");
                if (incoming_status.kind == PointerKind::DYNAMIC) {
                  if (incoming_status.dynamic_kind == old_incoming_kind) {
                    // up to date, nothing to do
                  } else {
                    // a different dynamic status than we had previously.
                    // (or maybe we previously had a constant mask here.)
                    // Just change the status_phi in place.
                    status_phi->setIncomingValueForBlock(incoming_bb, incoming_status.dynamic_kind);
                  }
                } else {
                  Value* new_dynamic_kind = incoming_status.to_dynamic_kind_mask(Builder);
                  assert(new_dynamic_kind && "when pointer_encoding_is_complete, we shouldn't have dynamic_kind == NULL");
                  // since incoming_status.kind isn't DYNAMIC, new_dynamic_kind
                  // should be a ConstantInt
                  ConstantInt* new_mask = cast<ConstantInt>(new_dynamic_kind);
                  if (const ConstantInt* old_mask = cast<const ConstantInt>(old_incoming_kind)) {
                    if (new_mask->getValue() == old_mask->getValue()) {
                      // up to date, nothing to do
                    } else {
                      // a different constant mask than we had previously.
                      // Just change the status_phi in place.
                      status_phi->setIncomingValueForBlock(incoming_bb, new_mask);
                    }
                  } else {
                    // a constant mask, where previously we had a dynamic mask.
                    // Just change the status_phi in place.
                    status_phi->setIncomingValueForBlock(incoming_bb, new_mask);
                  }
                }
              }
              // now we're done
              ptr_statuses.mark_as(&phi, PointerStatus::dynamic(status_phi));
            } else {
              // ok, compute the result status fresh.
              // If all incoming status are statically known (i.e. not dynamic),
              // then the result status is just the merger of the statuses of all
              // the inputs in their corresponding blocks.
              // But if some incoming status are dynamic, and if
              // `pointer_encoding_is_complete` (which guarantees that all
              // `dynamic_kind`s are filled-in, i.e., non-NULL), then we'll just
              // add a new PHI to select the correct dynamic status.
              // (We could theoretically use a PHI even for the all-static case.
              // This would have the advantage of path-sensitive status, but the
              // disadvantage of having a dynamic status when the merger approach
              // gives a statically-known status, albeit a more conservative one.
              // Currently we don't insert a PHI in the all-static case. But, if
              // we insert one, and then in a later iteration it becomes
              // all-static somehow, we still leave the PHI.)
              assert(incoming_statuses.size() >= 1);
              bool all_incoming_status_are_static = true;
              for (auto& pair : incoming_statuses) {
                const PointerStatus& incoming_status = pair.first;
                if (incoming_status.kind == PointerKind::DYNAMIC) {
                  all_incoming_status_are_static = false;
                  break;
                }
              }
              if (pointer_encoding_is_complete && !all_incoming_status_are_static) {
                PHINode* status_phi = Builder.CreatePHI(Builder.getInt64Ty(), phi.getNumIncomingValues());
                for (auto& pair : incoming_statuses) {
                  PointerStatus& incoming_status = pair.first;
                  BasicBlock* incoming_bb = pair.second;
                  Value* dynamic_kind = incoming_status.to_dynamic_kind_mask(Builder);
                  assert(dynamic_kind && "when pointer_encoding_is_complete, we shouldn't have dynamic_kind == NULL");
                  status_phi->addIncoming(dynamic_kind, incoming_bb);
                }
                assert(status_phi->isComplete());
                ptr_statuses.mark_as(&phi, PointerStatus::dynamic(status_phi));
                dynamic_phi_to_status_phi[&phi] = status_phi;
              } else {
                PointerStatus merged_status = PointerStatus::clean();
                for (auto& pair : incoming_statuses) {
                  PointerStatus& incoming_status = pair.first;
                  merged_status = PointerStatus::merge(merged_status, incoming_status, &inst);
                }
                ptr_statuses.mark_as(&phi, merged_status);
              }
            }
            // also propagate bounds info
            IRBuilder<> Builder(&block, block.getFirstInsertionPt());
            const BoundsInfo& prev_iteration_binfo = bounds_info.lookup(&phi);
            assert(incoming_binfos.size() >= 1);
            bool any_incoming_bounds_are_dynamic = false;
            bool any_incoming_bounds_are_unknown = false;
            bool any_incoming_bounds_are_notdefinedyet = false;
            for (auto& pair : incoming_binfos) {
              const BoundsInfo* incoming_binfo = pair.first;
              if (incoming_binfo->is_dynamic()) {
                any_incoming_bounds_are_dynamic = true;
              }
              if (incoming_binfo->get_kind() == BoundsInfo::UNKNOWN) {
                any_incoming_bounds_are_unknown = true;
              }
              if (incoming_binfo->get_kind() == BoundsInfo::NOTDEFINEDYET) {
                any_incoming_bounds_are_notdefinedyet = true;
              }
            }
            if (any_incoming_bounds_are_unknown) {
              bounds_info[&phi] = BoundsInfo::unknown();
            } else if (any_incoming_bounds_are_dynamic) {
              if (any_incoming_bounds_are_notdefinedyet) {
                // in this case, just mark UNKNOWN for this iteration; we'll
                // insert PHIs and compute the proper dynamic bounds in the next
                // iteration, when everything is defined
                bounds_info[&phi] = BoundsInfo::unknown();
              } else {
                // in this case, we'll use PHIs to select the proper dynamic
                // `base` and `max`, much as we used PHIs for the dynamic_kind
                // above.
                // Of course, if we already inserted PHIs in a previous iteration,
                // let's not insert them again.
                PHINode* base_phi = NULL;
                PHINode* max_phi = NULL;
                if (const BoundsInfo::DynamicBoundsInfo* prev_iteration_dyninfo = prev_iteration_binfo.dynamic_info()) {
                  if ((base_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->base.ptr))) {
                    assert(prev_iteration_dyninfo->base.offset == 0);
                    assert(incoming_binfos.size() == base_phi->getNumIncomingValues());
                    for (auto& pair : incoming_binfos) {
                      const BoundsInfo* incoming_binfo = pair.first;
                      BasicBlock* incoming_bb = pair.second;
                      const Value* old_base = base_phi->getIncomingValueForBlock(incoming_bb);
                      switch (incoming_binfo->get_kind()) {
                        case BoundsInfo::NOTDEFINEDYET:
                        case BoundsInfo::UNKNOWN:
                        case BoundsInfo::INFINITE:
                          llvm_unreachable("Bad incoming_binfo.kind here");
                        case BoundsInfo::STATIC:
                          // for now we just assume that static bounds don't
                          // change from iteration to iteration
                          break;
                        case BoundsInfo::DYNAMIC:
                        case BoundsInfo::DYNAMIC_MERGED:
                        {
                          const BoundsInfo::DynamicBoundsInfo* incoming_dyninfo = incoming_binfo->dynamic_info();
                          if (incoming_dyninfo->base.offset != 0) {
                            // need to recalculate base_phi
                            base_phi = NULL;
                            break;
                          }
                          const Value* incoming_base = incoming_dyninfo->base.ptr;
                          if (incoming_base != old_base) {
                            // need to recalculate base_phi
                            base_phi = NULL;
                            break;
                          }
                          // if we get here, this incoming block is still good and doesn't need recalculating
                          break;
                        }
                        default:
                          llvm_unreachable("Missing BoundsInfo.kind case");
                      }
                      if (!base_phi) break;
                    }
                  } else {
                    // prev_iteration base was not a phi. we'll have to insert a fresh phi
                  }
                  if ((max_phi = dyn_cast<PHINode>(prev_iteration_dyninfo->max.ptr))) {
                    assert(prev_iteration_dyninfo->max.offset == 0);
                    assert(incoming_binfos.size() == max_phi->getNumIncomingValues());
                    for (auto& pair : incoming_binfos) {
                      const BoundsInfo* incoming_binfo = pair.first;
                      BasicBlock* incoming_bb = pair.second;
                      const Value* old_max = max_phi->getIncomingValueForBlock(incoming_bb);
                      switch (incoming_binfo->get_kind()) {
                        case BoundsInfo::NOTDEFINEDYET:
                        case BoundsInfo::UNKNOWN:
                        case BoundsInfo::INFINITE:
                          llvm_unreachable("Bad incoming_binfo.kind here");
                        case BoundsInfo::STATIC:
                          // for now we just assume that static bounds don't
                          // change from iteration to iteration
                          break;
                        case BoundsInfo::DYNAMIC:
                        case BoundsInfo::DYNAMIC_MERGED:
                        {
                          const BoundsInfo::DynamicBoundsInfo* incoming_dyninfo = incoming_binfo->dynamic_info();
                          if (incoming_dyninfo->max.offset != 0) {
                            // need to recalculate max_phi
                            max_phi = NULL;
                            break;
                          }
                          const Value* incoming_max = incoming_dyninfo->max.ptr;
                          if (incoming_max != old_max) {
                            // need to recalculate max_phi
                            max_phi = NULL;
                            break;
                          }
                          // if we get here, this incoming block is still good and doesn't need recalculating
                          break;
                        }
                        default:
                          llvm_unreachable("Missing BoundsInfo.kind case");
                      }
                      if (!max_phi) break;
                    }

                  } else {
                    // prev_iteration max was not a phi. we'll have to insert a fresh phi
                  }
                } else {
                  // prev_iteration boundsinfo was not dynamic. we'll have to insert fresh phis
                }
                if (!base_phi) {
                  base_phi = Builder.CreatePHI(Builder.getInt8PtrTy(), phi.getNumIncomingValues());
                  bounds_insts.insert(base_phi);
                  for (auto& pair : incoming_binfos) {
                    const BoundsInfo* incoming_binfo = pair.first;
                    BasicBlock* incoming_bb = pair.second;
                    // if dynamic instructions are necessary to compute phi
                    // incoming value, insert them at the end of the
                    // corresponding block, not here
                    IRBuilder<> IncomingBlockBuilder(incoming_bb->getTerminator());
                    Value* base = incoming_binfo->base_as_llvm_value(&phi, IncomingBlockBuilder, bounds_insts);
                    assert(base);
                    base_phi->addIncoming(base, incoming_bb);
                  }
                  assert(base_phi->isComplete());
                }
                if (!max_phi) {
                  max_phi = Builder.CreatePHI(Builder.getInt8PtrTy(), phi.getNumIncomingValues());
                  bounds_insts.insert(max_phi);
                  for (auto& pair : incoming_binfos) {
                    const BoundsInfo* incoming_binfo = pair.first;
                    BasicBlock* incoming_bb = pair.second;
                    // if dynamic instructions are necessary to compute phi
                    // incoming value, insert them at the end of the
                    // corresponding block, not here
                    IRBuilder<> IncomingBlockBuilder(incoming_bb->getTerminator());
                    Value* max = incoming_binfo->max_as_llvm_value(&phi, IncomingBlockBuilder, bounds_insts);
                    assert(max);
                    max_phi->addIncoming(max, incoming_bb);
                  }
                  assert(max_phi->isComplete());
                }
                bounds_info[&phi] = BoundsInfo::dynamic_bounds(base_phi, max_phi);
              }
            } else {
              // no incoming bounds are dynamic. let's just merge them statically
              bool any_merge_inputs_changed = false;
              if (prev_iteration_binfo.get_kind() != BoundsInfo::DYNAMIC_MERGED)
                any_merge_inputs_changed = true;
              if (prev_iteration_binfo.merge_inputs.size() != incoming_binfos.size())
                any_merge_inputs_changed = true;
              if (!any_merge_inputs_changed) {
                for (unsigned i = 0; i < incoming_binfos.size(); i++) {
                  if (*prev_iteration_binfo.merge_inputs[i] != *incoming_binfos[i].first) {
                    any_merge_inputs_changed = true;
                    break;
                  }
                }
              }
              if (any_merge_inputs_changed) {
                // have to update the boundsinfo
                BoundsInfo merged_binfo = BoundsInfo::infinite(); // just the initial value we start the merge with
                assert(phi.getNumIncomingValues() >= 1);
                for (auto& pair : incoming_binfos) {
                  const BoundsInfo* binfo = pair.first;
                  merged_binfo = BoundsInfo::merge(merged_binfo, *binfo, &phi, Builder, bounds_insts);
                }
                bounds_info[&phi] = merged_binfo;
              }
            }
            for (auto& pair : incoming_binfos) {
              const BoundsInfo* binfo = pair.first;
              delete binfo;
            }
          }
          break;
        }
        case Instruction::IntToPtr: {
          const IntToPtrInst& inttoptr = cast<IntToPtrInst>(inst);
          auto it = inttoptr_status_and_bounds_overrides.find(&inttoptr);
          if (it != inttoptr_status_and_bounds_overrides.end()) {
            // there's an override in place for this IntToPtr.
            // Ignore it for stats purposes, and copy the status and boundsinfo
            // from the indicated Value.
            // See notes on `inttoptr_status_and_bounds_overrides`.
            PointerStatus status = ptr_statuses.getStatus(it->getSecond());
            assert(status.kind != PointerKind::NOTDEFINEDYET);
            ptr_statuses.mark_as(&inttoptr, status);
            bounds_info[&inttoptr] = bounds_info[it->getSecond()];
          } else {
            // no override in place for this IntToPtr.
            // count this for stats, and then mark it as `inttoptr_kind`
            static_results.inttoptrs++;
            if (dynamic_results) {
              incrementGlobalCounter(dynamic_results->inttoptrs, &inst);
            }
            ptr_statuses.mark_as(&inttoptr, inttoptr_kind);
            // if we're considering it a clean ptr, then also assume it
            // is valid for the entire size of the data its type claims it
            // points to
            if (inttoptr_kind == PointerKind::CLEAN) {
              PointerType* resultType = cast<PointerType>(inttoptr.getType());
              auto allocationSize = DL.getTypeStoreSize(resultType->getElementType()).getFixedSize();
              bounds_info[&inttoptr] = BoundsInfo::static_bounds(
                zero, APInt(/* bits = */ 64, /* val = */ allocationSize - 1)
              );
            }
          }
          break;
        }
        case Instruction::Call:
        case Instruction::CallBr:
        case Instruction::Invoke:
        // all three of these are instructions which call functions, and we
        // handle them the same
        {
          CallBase& call = cast<CallBase>(inst);
          // count call arguments for stats purposes, if appropriate
          if (shouldCountCallForStatsPurposes(call)) {
            for (const Use& arg : call.args()) {
              const Value* value = arg.get();
              if (value->getType()->isPointerTy()) {
                COUNT_OP_AS_STATUS(passed_ptrs, ptr_statuses.getStatus(value), &inst, "Call argument is a pointer");
              }
            }
          }
          // now classify the returned pointer, if the return value is a pointer
          if (call.getType()->isPointerTy()) {
            // If this is an allocating call (eg, a call to `malloc`), then the
            // returned pointer is CLEAN
            if (isAllocatingCall(call)) {
              ptr_statuses.mark_clean(&call);
              assert(call.getNumArgOperands() == 1);
              Value* allocationBytes = call.getArgOperand(0);
              if (ConstantInt* allocationBytes_const = dyn_cast<ConstantInt>(allocationBytes)) {
                // allocating a constant number of bytes.
                // we know the bounds of the allocation statically.
                bounds_info[&call] = BoundsInfo::static_bounds(
                  zero, allocationBytes_const->getValue() - 1
                );
              } else {
                // allocating a dynamic number of bytes.
                // We need a dynamic addition instruction to compute the upper
                // bound. Only insert that the first time -- the bounds info
                // here should not change from iteration to iteration
                if (bounds_info.count(&call) == 0) {
                  IRBuilder<> AfterCall(&block);
                  setInsertPointToAfterInst(AfterCall, &call);
                  Value* callPlusBytes = AfterCall.CreateAdd(&call, allocationBytes);
                  if (callPlusBytes != &call) {
                    bounds_insts.insert(cast<Instruction>(callPlusBytes));
                  }
                  Value* max = AfterCall.CreateSub(callPlusBytes, AfterCall.getInt64(1));
                  if (max != callPlusBytes) {
                    bounds_insts.insert(cast<Instruction>(max));
                  }
                  bounds_info[&call] = BoundsInfo::dynamic_bounds(&call, max);
                }
              }
            } else {
              // For now, mark pointers returned from other calls as UNKNOWN
              ptr_statuses.mark_unknown(&call);
              bounds_info[&call] = BoundsInfo::unknown();
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
          bounds_info[&inst] = BoundsInfo::unknown();
          break;
        }
        case Instruction::ExtractElement: {
          // same comments apply as for ExtractValue, basically
          ptr_statuses.mark_unknown(&inst);
          bounds_info[&inst] = BoundsInfo::unknown();
          break;
        }
        case Instruction::Ret: {
          const ReturnInst& ret = cast<ReturnInst>(inst);
          const Value* retval = ret.getReturnValue();
          if (retval && retval->getType()->isPointerTy()) {
            COUNT_OP_AS_STATUS(returned_ptrs, ptr_statuses.getStatus(retval), &inst, "Returning a pointer");
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
    DEBUG_WITH_TYPE("DMS-block-stats", dbgs() << "DMS:   at end of block, we now have " << ptr_statuses.describe() << "\n");
    const bool changed = !ptr_statuses.isEqualTo(pbs->ptrs_end);
    if (changed) {
      DEBUG_WITH_TYPE("DMS-block-stats", dbgs() << "DMS:   this was a change\n");
    }
    pbs->ptrs_end = std::move(ptr_statuses);
    pbs->static_results = static_results;
    return AnalyzeBlockResult { changed, static_results };
  }

  /// If necessary, add a dynamic spatial safety check for the dereference of
  /// `addr`, assuming `addr` has status `status`.
  /// If dynamic instructions need to be inserted, use `Builder`.
  ///
  /// Currently this assumes that dereferencing BLEMISHED16 pointers does not
  /// need a SW check, but all other BLEMISHED pointers need checks.
  void maybeAddSpatialSWCheck(const Value* addr, PointerStatus status, IRBuilder<>& Builder) {
    switch (status.kind) {
      case PointerKind::CLEAN:
        // no check required
        return;
      case PointerKind::BLEMISHED16:
        // no check required
        return;
      case PointerKind::BLEMISHED32:
      case PointerKind::BLEMISHED64:
      case PointerKind::BLEMISHEDCONST:
      case PointerKind::DIRTY: {
        const BoundsInfo& binfo = bounds_info.lookup(addr);
        switch (binfo.get_kind()) {
          case BoundsInfo::UNKNOWN:
            dbgs() << "warning: bounds info unknown for " << addr->getNameOrAsOperand() << " even though it needs a bounds check. Unsafely omitting the bounds check.\n";
            return;
          case BoundsInfo::INFINITE:
            // no check required
            return;
          case BoundsInfo::STATIC: {
            const BoundsInfo::StaticBoundsInfo* static_info = binfo.static_info();
            if (static_info->low_offset.isStrictlyPositive()) {
              // invalid pointer: too low
              insertBoundsCheckFail(Builder);
            } else if (static_info->high_offset.isNegative()) {
              // invalid pointer: too high
              insertBoundsCheckFail(Builder);
            }
            break;
          }
          case BoundsInfo::DYNAMIC:
          case BoundsInfo::DYNAMIC_MERGED:
          {
            llvm_unreachable("unimplemented: spatial check with dynamic bounds info");
          }
          default:
            llvm_unreachable("Missing BoundsInfo.kind case");
        }
        break;
      }
      case PointerKind::DYNAMIC: {
        const BoundsInfo& binfo = bounds_info.lookup(addr);
        switch (binfo.get_kind()) {
          case BoundsInfo::UNKNOWN:
            dbgs() << "warning: bounds info unknown for " << addr->getNameOrAsOperand() << " even though it may need a bounds check, depending on dynamic kind. Unsafely omitting the bounds check.\n";
            break;
          case BoundsInfo::INFINITE:
            // no check required
            break;
          case BoundsInfo::STATIC: {
            const BoundsInfo::StaticBoundsInfo* static_info = binfo.static_info();
            if (
              static_info->low_offset.isStrictlyPositive() /* invalid pointer: too low */
              || static_info->high_offset.isNegative() /* invalid pointer: too high */
            ) {
              // we check the dynamic kind, if it is DYN_CLEAN or
              // DYN_BLEMISHED16 then we do nothing, else we SW-fail
              BasicBlock* bb = Builder.GetInsertBlock();
              BasicBlock::iterator I = Builder.GetInsertPoint();
              BasicBlock* new_bb = bb->splitBasicBlock(I);
              // at this point, `bb` holds everything before the pointer
              // dereference, and an unconditional-br terminator to `new_bb`.
              // `new_bb` holds the dereference and everything following,
              // including the old terminator.
              // To be safe, we assume that `Builder` is invalidated by the
              // above operation (docs say that the iterator `I` is
              // invalidated).
              BasicBlock* boundsfail = BasicBlock::Create(F.getContext(), "", &F);
              IRBuilder<> BoundsFailBuilder(boundsfail, boundsfail->getFirstInsertionPt());
              insertBoundsCheckFail(BoundsFailBuilder);
              // replace `bb`'s terminator with a condbr jumping to either
              // `boundsfail` or `new_bb` as appropriate
              BasicBlock::iterator bbend = bb->getTerminator()->eraseFromParent();
              IRBuilder<> Builder(bb, bbend);
              Value* doesnt_need_check = Builder.CreateLogicalOr(
                Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::clean)),
                Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16))
              );
              Builder.CreateCondBr(doesnt_need_check, new_bb, boundsfail);
            }
            break;
          }
          case BoundsInfo::DYNAMIC:
          case BoundsInfo::DYNAMIC_MERGED:
          {
            llvm_unreachable("unimplemented: spatial check with dynamic bounds info and dynamic kind");
          }
          default:
            llvm_unreachable("Missing BoundsInfo.kind case");
        }
        break;
      }
      case PointerKind::UNKNOWN:
        llvm_unreachable("unimplemented: bounds check on unknown-status pointer");
      case PointerKind::NOTDEFINEDYET:
        llvm_unreachable("trying to bounds check on pointer with NOTDEFINEDYET status");
      default:
        llvm_unreachable("Missing PointerKind case");
    }
  }

  /// Insert dynamic instructions indicating a bounds-check failure, at the
  /// program point indicated by the given `Builder`
  void insertBoundsCheckFail(IRBuilder<>& Builder) {
    Module* mod = F.getParent();
    FunctionType* AbortTy = FunctionType::get(Builder.getVoidTy(), /* IsVarArgs = */ false);
    FunctionCallee Abort = mod->getOrInsertFunction("abort", AbortTy);
    Builder.CreateCall(Abort);
    Builder.CreateUnreachable();
  }

  DynamicResults initializeDynamicResults() {
    DynamicCounts load_addrs = initializeDynamicCounts("__DMS_load_addrs");
    DynamicCounts store_addrs = initializeDynamicCounts("__DMS_store_addrs");
    DynamicCounts store_vals = initializeDynamicCounts("__DMS_store_vals");
    DynamicCounts passed_ptrs = initializeDynamicCounts("__DMS_passed_ptrs");
    DynamicCounts returned_ptrs = initializeDynamicCounts("__DMS_returned_ptrs");
    DynamicCounts pointer_arith_const = initializeDynamicCounts("__DMS_pointer_arith_const");
    Constant* inttoptrs = findOrCreateGlobalCounter("__DMS_inttoptrs");
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
      gv->setInitializer(ConstantInt::get(ctx, zero));
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

  // Inject an instruction sequence to increment the appropriate global counter
  // based on the `PointerStatus`. This is to be used when (and only when) the
  // kind is `DYNAMIC`.
  void incrementGlobalCounterForDynKind(DynamicCounts& dyn_counts, PointerStatus& status, Instruction* BeforeInst) {
    IRBuilder<> Builder(BeforeInst);
    Type* i64ty = Builder.getInt64Ty();
    Constant* null = Constant::getNullValue(dyn_counts.clean->getType());
    Value* GlobalCounter = Builder.CreateSelect(
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::clean)),
      dyn_counts.clean,
      null);
    GlobalCounter = Builder.CreateSelect(
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16)),
      dyn_counts.blemished16,
      GlobalCounter);
    GlobalCounter = Builder.CreateSelect(
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished_other)),
      dyn_counts.blemishedconst,  // conservative, as we don't know which BLEMISHED category to use
      GlobalCounter);
    GlobalCounter = Builder.CreateSelect(
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::dirty)),
      dyn_counts.dirty,
      GlobalCounter);
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
    if (mod->getFunction("__DMS_output_wrapper")) {
      return;
    }

    std::string output = "";
    output += "================\n";
    output += "DMS dynamic counts for " + mod->getName().str() + ":\n";
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
    Constant* OutputStr = createGlobalConstStr(mod, "__DMS_output_str", output.c_str());

    // Create a void function which calls printf() or fprintf() to print the
    // output
    Type* i8ty = IntegerType::getInt8Ty(ctx);
    Type* i8StarTy = PointerType::getUnqual(i8ty);
    Type* i32ty = IntegerType::getInt32Ty(ctx);
    Type* i32StarTy = PointerType::getUnqual(i32ty);
    Type* i64ty = IntegerType::getInt64Ty(ctx);
    FunctionType* WrapperTy = FunctionType::get(Type::getVoidTy(ctx), {}, false);
    Function* Wrapper_func = cast<Function>(mod->getOrInsertFunction("__DMS_output_wrapper", WrapperTy).getCallee());
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
      auto file_str = "./dms_dynamic_counts/" + modNameWithDots;
      Constant* DirStr = createGlobalConstStr(mod, "__DMS_dir_str", "./dms_dynamic_counts");
      Constant* FileStr = createGlobalConstStr(mod, "__DMS_file_str", file_str.c_str());
      Constant* ModeStr = createGlobalConstStr(mod, "__DMS_mode_str", "a");
      Constant* PerrorStr = createGlobalConstStr(mod, "__DMS_perror_str", ("Failed to open " + file_str).c_str());
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
      llvm_unreachable("unexpected print_type\n");
    }

    // Inject this wrapper function into the GlobalDtors for the module
    appendToGlobalDtors(*mod, Wrapper_func, /* Priority = */ 0);
  }
};

PreservedAnalyses StaticDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DMSAnalysis analysis = DMSAnalysis(F, FAM, true, PointerKind::CLEAN, false);
  DMSAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // StaticDMSPass only analyzes the IR and doesn't make any changes, so all
  // analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses ParanoidStaticDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DMSAnalysis analysis = DMSAnalysis(F, FAM, false, PointerKind::DIRTY, false);
  DMSAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // ParanoidStaticDMSPass only analyzes the IR and doesn't make any changes,
  // so all analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses DynamicDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // Don't do any analysis or instrumentation on the special function __DMS_output_wrapper
  if (F.getName() == "__DMS_output_wrapper") {
    return PreservedAnalyses::all();
  }

  DMSAnalysis analysis = DMSAnalysis(F, FAM, true, PointerKind::CLEAN, true);
  analysis.run();
  analysis.instrument(DMSAnalysis::DynamicPrintType::TOFILE);

  // For now we conservatively just tell LLVM that no analyses are preserved.
  // It seems that many existing LLVM passes also just use
  // PreservedAnalyses::none() when they make any change, so we assume this is
  // reasonable.
  return PreservedAnalyses::none();
}

PreservedAnalyses DynamicStdoutDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // Don't do any analysis or instrumentation on the special function __DMS_output_wrapper
  if (F.getName() == "__DMS_output_wrapper") {
    return PreservedAnalyses::all();
  }

  DMSAnalysis analysis = DMSAnalysis(F, FAM, true, PointerKind::CLEAN, true);
  analysis.run();
  analysis.instrument(DMSAnalysis::DynamicPrintType::STDOUT);

  // For now we conservatively just tell LLVM that no analyses are preserved.
  // It seems that many existing LLVM passes also just use
  // PreservedAnalyses::none() when they make any change, so we assume this is
  // reasonable.
  return PreservedAnalyses::none();
}

PreservedAnalyses BoundsChecksDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DMSAnalysis analysis = DMSAnalysis(F, FAM, true, PointerKind::CLEAN, true);
  analysis.run();
  analysis.addSpatialSafetySWChecks();

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
static GEPResultClassification classifyGEPResult(
  const GetElementPtrInst &gep,
  const PointerStatus input_status,
  const DataLayout &DL,
  const bool trustLLVMStructTypes,
  const APInt* override_constant_offset
) {
  assert(input_status.kind != PointerKind::NOTDEFINEDYET && "Shouldn't call classifyGEPResult() with NOTDEFINEDYET input_ptr");
  GEPResultClassification grc;
  grc.offset_is_constant = false;
  grc.constant_offset = zero; // `constant_offset` is only valid if `offset_is_constant`
  if (override_constant_offset == NULL) {
    grc.offset_is_constant = gep.accumulateConstantOffset(DL, grc.constant_offset);
  } else {
    grc.offset_is_constant = true;
    grc.constant_offset = *override_constant_offset;
  }

  if (gep.hasAllZeroIndices()) {
    // result of a GEP with all zeroes as indices, is the same as the input pointer.
    assert(grc.offset_is_constant && grc.constant_offset == zero && "If all indices are constant 0, then the total offset should be constant 0");
    grc.classification = input_status;
    grc.trustworthy_struct_offset = false;
    return grc;
  }
  if (trustLLVMStructTypes && areAllIndicesTrustworthy(gep)) {
    // nonzero offset, but "trustworthy" offset.
    assert(grc.offset_is_constant);
    grc.trustworthy_struct_offset = true;
    switch (input_status.kind) {
      case PointerKind::CLEAN: {
        grc.classification = PointerStatus::clean();
        return grc;
      }
      case PointerKind::UNKNOWN: {
        grc.classification = PointerStatus::unknown();
        return grc;
      }
      case PointerKind::DIRTY: {
        grc.classification = PointerStatus::dirty();
        return grc;
      }
      case PointerKind::BLEMISHED16:
      case PointerKind::BLEMISHED32:
      case PointerKind::BLEMISHED64:
      case PointerKind::BLEMISHEDCONST: {
        // fall through. "Trustworthy" offset from a blemished pointer still needs
        // to increase the blemished-ness of the pointer, as handled below.
        break;
      }
      case PointerKind::DYNAMIC: {
        if (input_status.dynamic_kind == NULL) {
          grc.classification = PointerStatus::dynamic(NULL);
        } else {
          // "trustworthy" offset from clean is clean, from dirty is dirty,
          // from BLEMISHED16 is arbitrarily blemished, and from arbitrarily
          // blemished is still arbitrarily blemished.
          IRBuilder<> Builder((GetElementPtrInst*)&gep); // cast to discard const. We should be able to insert stuff before a const instruction.
          Value* dynamic_kind = Builder.CreateSelect(
            Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16)),
            Builder.getInt64(DynamicKindMasks::blemished_other),
            input_status.dynamic_kind
          );
          grc.classification = PointerStatus::dynamic(dynamic_kind);
        }
        return grc;
      }
      case PointerKind::NOTDEFINEDYET: {
        llvm_unreachable("GEP on a pointer with no status");
      }
      default:
        llvm_unreachable("PointerKind case not handled");
    }
  }

  // if we get here, we don't have a zero constant offset. Either it's a nonzero constant,
  // or a nonconstant.
  if (grc.offset_is_constant) {
    switch (input_status.kind) {
      case PointerKind::CLEAN: {
        // This GEP adds a constant but nonzero amount to a CLEAN
        // pointer. The result is some flavor of BLEMISHED depending
        // on how far the pointer arithmetic goes.
        if (grc.constant_offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          grc.classification = PointerStatus::blemished16();
          return grc;
        } else if (grc.constant_offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (grc.constant_offset.ule(APInt(/* bits = */ 64, /* val = */ 64))) {
          grc.classification = PointerStatus::blemished64();
          return grc;
        } else {
          // offset is constant, but larger than 64 bytes
          grc.classification = PointerStatus::blemishedconst();
          return grc;
        }
        break;
      }
      case PointerKind::BLEMISHED16: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHED16 pointer. The result is some flavor of BLEMISHED
        // depending on how far the pointer arithmetic goes.
        if (grc.constant_offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          // Conservatively, the total offset can't exceed 32
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (grc.constant_offset.ule(APInt(/* bits = */ 64, /* val = */ 48))) {
          // Conservatively, the total offset can't exceed 64
          grc.classification = PointerStatus::blemished64();
          return grc;
        } else {
          // offset is constant, but may be larger than 64 bytes
          grc.classification = PointerStatus::blemishedconst();
          return grc;
        }
        break;
      }
      case PointerKind::BLEMISHED32: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHED32 pointer. The result is some flavor of BLEMISHED
        // depending on how far the pointer arithmetic goes.
        if (grc.constant_offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
          // Conservatively, the total offset can't exceed 64
          grc.classification = PointerStatus::blemished64();
          return grc;
        } else {
          // offset is constant, but may be larger than 64 bytes
          grc.classification = PointerStatus::blemishedconst();
          return grc;
        }
        break;
      }
      case PointerKind::BLEMISHED64: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHED64 pointer. The result is BLEMISHEDCONST, as we
        // can't prove the total constant offset remains 64 or less.
        grc.classification = PointerStatus::blemishedconst();
        return grc;
        break;
      }
      case PointerKind::BLEMISHEDCONST: {
        // This GEP adds a constant but nonzero amount to a
        // BLEMISHEDCONST pointer. The result is still BLEMISHEDCONST,
        // as the total offset is still a constant.
        grc.classification = PointerStatus::blemishedconst();
        return grc;
        break;
      }
      case PointerKind::DIRTY: {
        // result of a GEP with any nonzero indices, on a DIRTY or
        // UNKNOWN pointer, is always DIRTY.
        grc.classification = PointerStatus::dirty();
        return grc;
        break;
      }
      case PointerKind::UNKNOWN: {
        // result of a GEP with any nonzero indices, on a DIRTY or
        // UNKNOWN pointer, is always DIRTY.
        grc.classification = PointerStatus::dirty();
        return grc;
        break;
      }
      case PointerKind::DYNAMIC: {
        // This GEP adds a constant but nonzero amount to a DYNAMIC pointer.
        if (input_status.dynamic_kind == NULL) {
          grc.classification = PointerStatus::dynamic(NULL);
        } else {
          // We need to dynamically check the kind in order to classify the
          // result.
          IRBuilder<> Builder((GetElementPtrInst*)&gep); // cast to discard const. We should be able to insert stuff before a const instruction.
          Value* is_clean = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::clean));
          Value* is_blem16 = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16));
          Value* is_blemother = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished_other));
          Value* dynamic_kind = Builder.getInt64(DynamicKindMasks::dirty);
          dynamic_kind = Builder.CreateSelect(
            is_clean,
            (grc.constant_offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) ?
              Builder.getInt64(DynamicKindMasks::blemished16) : // offset <= 16 from a dynamically clean pointer
              Builder.getInt64(DynamicKindMasks::blemished_other), // offset >16 from a dynamically clean pointer
            dynamic_kind
          );
          dynamic_kind = Builder.CreateSelect(
            Builder.CreateLogicalOr(is_blem16, is_blemother),
            Builder.getInt64(DynamicKindMasks::blemished_other), // any offset from any blemished has to be blemished_other, as we can't prove it stays within blemished16
            dynamic_kind
          );
          // the case where the kind was DYN_DIRTY is implicitly handled by the
          // default value of `dynamic_kind`. Result is still DYN_DIRTY in that
          // case.
          grc.classification = PointerStatus::dynamic(dynamic_kind);
        }
        return grc;
        break;
      }
      case PointerKind::NOTDEFINEDYET: {
        llvm_unreachable("GEP on a pointer with no status");
        break;
      }
      default: {
        llvm_unreachable("Missing PointerKind case");
        break;
      }
    }
  } else {
    // offset is not constant; so, result is dirty
    grc.classification = PointerStatus::dirty();
    return grc;
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

/// The given `Builder` will now be ready to insert instructions _after_ the
/// given `inst`
static void setInsertPointToAfterInst(IRBuilder<>& Builder, Instruction* inst) {
  Builder.SetInsertPoint(inst);
  BasicBlock* bb = Builder.GetInsertBlock();
  auto ip = Builder.GetInsertPoint();
  ip++;
  Builder.SetInsertPoint(bb, ip);
}

static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep) {
  DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "Analyzing the following gep:\n");
  DEBUG_WITH_TYPE("DMS-trustworthy-indices", gep.dump());
  Type* current_ty = gep.getPointerOperandType();
  SmallVector<Constant*, 8> seen_indices;
  for (const Use& idx : gep.indices()) {
    if (!current_ty) {
      LLVM_DEBUG(dbgs() << "current_ty is null - probably getIndexedType() returned null\n");
      return false;
    }
    if (ConstantInt* c = dyn_cast<ConstantInt>(idx.get())) {
      DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "Encountered constant index " << c->getSExtValue() << "\n");
      DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "Current ty is " << *current_ty << "\n");
      seen_indices.push_back(cast<Constant>(c));
      if (c->isZero()) {
        // zero is always trustworthy
        DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "zero is always trustworthy\n");
      } else {
        // constant, nonzero index
        if (seen_indices.size() == 1) {
          // the first time is just selecting the element of the implied array.
          DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "indexing into an implicit array is not trustworthy\n");
          return false;
        }
        const PointerType* current_ty_as_ptrtype = cast<const PointerType>(current_ty);
        const Type* current_pointee_ty = current_ty_as_ptrtype->getElementType();
        DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "Current pointee ty is " << *current_pointee_ty << "\n");
        if (current_pointee_ty->isStructTy()) {
          // trustworthy
          DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "indexing into a struct ty is trustworthy\n");
        } else if (current_pointee_ty->isArrayTy()) {
          // not trustworthy
          DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "indexing into an array ty is not trustworthy\n");
          return false;
        } else {
          // implicit array type. e.g., indexing into an i32*.
          DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "indexing into an implicit array is not trustworthy\n");
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

/// Is the given call one of the ones which we "should count" for stats
/// purposes?
/// (Calls to some LLVM intrinsics "shouldn't count" because they "aren't real",
/// i.e., won't appear in the final binary)
///
/// (When unsure, we conservatively return `true`)
static bool shouldCountCallForStatsPurposes(const CallBase &call) {
  Function* callee = call.getCalledFunction();
  if (!callee) {
    // indirect call. when in doubt, count it.
    return true;
  }
  if (!callee->hasName()) {
    // call to anonymous function. when in doubt, count it.
    return true;
  }
  StringRef name = callee->getName();
  if (name.startswith("llvm.lifetime")
    || name.startswith("llvm.invariant")
    || name.startswith("llvm.launder.invariant")
    || name.startswith("llvm.strip.invariant")
    || name.startswith("llvm.dbg")
    || name.startswith("llvm.expect"))
  {
    // these LLVM intrinsics shouldn't be counted for stats purposes
    return false;
  }
  // count calls to all other functions
  return true;
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
