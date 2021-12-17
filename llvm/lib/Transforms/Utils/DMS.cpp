#include "llvm/Transforms/Utils/DMS.h"
#include "llvm/Transforms/Utils/DMS_BoundsInfo.h"
#include "llvm/Transforms/Utils/DMS_BoundsInfos.h"
#include "llvm/Transforms/Utils/DMS_common.h"
#include "llvm/Transforms/Utils/DMS_DynamicResults.h"
#include "llvm/Transforms/Utils/DMS_Induction.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"
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
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/Local.h"

#include <sstream>  // ostringstream

using namespace llvm;

#define DEBUG_TYPE "DMS"

const APInt zero = APInt(/* bits = */ 64, /* val = */ 0);
const APInt minusone = APInt(/* bits = */ 64, /* val = */ -1);

/// Return type of `mapsAreEqual`. Another struct that would ideally be
/// std::optional or std::variant, but we can't use C++17
template<typename K>
struct MapEqualityResult {
  /// Are the maps equal
  bool equal;
  /// If the maps aren't equal, but are the same size, here is one key they
  /// disagree on.  (If the maps are equal, this will be NULL. If the maps
  /// aren't equal because they're different sizes, this will also be NULL.)
  const K* disagreement_key;

  static MapEqualityResult is_equal() { return MapEqualityResult { true, NULL }; }
  static MapEqualityResult not_equal(const K& key) { return MapEqualityResult { false, &key }; }
  static MapEqualityResult not_same_size() { return MapEqualityResult { false, NULL }; }
};

template <typename K, typename V, unsigned N> static MapEqualityResult<K> mapsAreEqual(const SmallDenseMap<K, V, N> &A, const SmallDenseMap<K, V, N> &B);
static void describePointerList(const SmallVector<const Value*, 8>& ptrs, std::ostringstream& out, StringRef desc);
static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep);
static bool shouldCountCallForStatsPurposes(const CallBase &call);

/// Describes the classification of a GEP result, as determined by
/// `classifyGEPResult()`.
struct GEPResultClassification {
  /// Classification of the result of the given `gep`.
  PointerStatus classification;
  /// Is the total offset of the GEP a constant, and if so, what is that
  /// constant?
  /// (If `override_constant_offset` is non-NULL, this will simply reflect the
  /// values specified in the override.)
  GEPConstantOffset offset;
  /// If `offset` is constant but nonzero, do we consider it as zero anyways
  /// because it is a "trustworthy" struct offset?
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
static GEPResultClassification classifyGEPResult(
  GetElementPtrInst &gep,
  const PointerStatus input_status,
  const DataLayout &DL,
  const bool trust_llvm_struct_types,
  const APInt* override_constant_offset,
  DenseSet<const Instruction*>& added_insts
);

/// Same as `classifyGEPResult`, but caches its results. If you call this with
/// the same arguments multiple times, you'll get the same result back.
/// (Critically, this _won't_ insert dynamic instructions on subsequent calls
/// with the same arguments, even if the first call required inserting dynamic
/// instructions.)
static GEPResultClassification classifyGEPResult_cached(
  GetElementPtrInst &gep,
  const PointerStatus input_status,
  const DataLayout &DL,
  const bool trust_llvm_struct_types,
  const APInt* override_constant_offset,
  DenseSet<const Instruction*>& added_insts
);

/// Conceptually stores the PointerKind of all currently valid pointers at a
/// particular program point.
class PointerStatuses {
private:
  /// Maps a pointer to its status.
  /// Pointers not appearing in this map are considered NOTDEFINEDYET.
  /// As a corollary, hopefully all pointers which are currently live do appear
  /// in this map.
  SmallDenseMap<const Value*, PointerStatus, 8> map;

  /// Which block are these statuses for. Note, this doesn't specify whether the
  /// statuses are for the top, middle, or bottom of the block.
  BasicBlock& block;

  const DataLayout &DL;
  const bool trust_llvm_struct_types;

  /// Reference to the `added_insts` where we note any instructions added for
  /// bounds purposes. See notes on `added_insts` in `DMSAnalysis`
  DenseSet<const Instruction*>& added_insts;

  /// Reference to the `pointer_aliases` for this function; see notes there
  DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases;

public:
  PointerStatuses(
    BasicBlock& block,
    const DataLayout &DL,
    const bool trust_llvm_struct_types,
    DenseSet<const Instruction*>& added_insts,
    DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
  ) :
    block(block),
    DL(DL),
    trust_llvm_struct_types(trust_llvm_struct_types),
    added_insts(added_insts),
    pointer_aliases(pointer_aliases)
  {}

  PointerStatuses(const PointerStatuses& other)
    : map(other.map),
    block(other.block),
    DL(other.DL),
    trust_llvm_struct_types(other.trust_llvm_struct_types),
    added_insts(other.added_insts),
    pointer_aliases(other.pointer_aliases)
  {}

  PointerStatuses operator=(const PointerStatuses& other) {
    assert(&block == &other.block);
    assert(DL == other.DL);
    assert(trust_llvm_struct_types == other.trust_llvm_struct_types);
    assert(&added_insts == &other.added_insts);
    assert(&pointer_aliases == &other.pointer_aliases);
    map = other.map;
    return *this;
  }

  // Use this for any `kind` except NOTDEFINEDYET or DYNAMIC
  void mark_as(const Value* ptr, PointerKind kind) {
    // don't explicitly mark anything NOTDEFINEDYET - we reserve
    // "not in the map" to mean NOTDEFINEDYET
    assert(kind != PointerKind::NOTDEFINEDYET);
    // DYNAMIC has to be handled with the other overload of `mark_as`
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

  /// Get the status of `ptr`. If necessary, check its aliases, and those
  /// aliases' aliases, etc
  PointerStatus getStatus(const Value* ptr) const {
    SmallDenseSet<const Value*, 4> tried_ptrs;
    return getStatus_checking_aliases_except(ptr, tried_ptrs);
  }

  private:
  /// Get the status of `ptr`. If necessary, check its aliases, and those
  /// aliases' aliases, etc. However, don't recurse into any aliases listed in
  /// `norecurse`. (We use this to avoid infinite recursion.)
  PointerStatus getStatus_checking_aliases_except(const Value* ptr, SmallDenseSet<const Value*, 4>& norecurse) const {
    PointerStatus status = getStatus_noalias(ptr);
    if (status.kind != PointerKind::NOTDEFINEDYET) return status;
    // if status isn't defined, see if it's defined for any alias of this pointer
    norecurse.insert(ptr);
    for (const Value* alias : pointer_aliases[ptr]) {
      if (norecurse.insert(alias).second) {
        status = getStatus_checking_aliases_except(alias, norecurse);
      }
      if (status.kind != PointerKind::NOTDEFINEDYET) return status;
    }
    return status;
  }

  /// Get the status of `ptr`. This function doesn't consider aliases of `ptr`;
  /// if `ptr` itself doesn't have a status, returns `PointerStatus::notdefinedyet()`.
  PointerStatus getStatus_noalias(const Value* ptr) const {
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
      } else if (isa<GlobalValue>(constant)) {
        // global values can be considered CLEAN
        // usually they'll be marked CLEAN in the actual `map`, but there are
        // edge cases, such as when a PHI gets status in a predecessor block but
        // the predecessor block hasn't been processed yet (eg due to a loop)
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
            Instruction* inst = expr->getAsInstruction();
            GetElementPtrInst* gepinst = cast<GetElementPtrInst>(inst);
            return classifyGEPResult_cached(*gepinst, getStatus(gepinst->getPointerOperand()), DL, trust_llvm_struct_types, NULL, added_insts).classification;
          }
          case Instruction::IntToPtr: {
            // if it's IntToPtr of zero, or any other number < 4K (corresponding
            // to the first page of memory, which is unmapped), we can treat it
            // as CLEAN, just like the null pointer
            if (const ConstantInt* cint = dyn_cast<ConstantInt>(expr->getOperand(0))) {
              if (cint->getValue().ult(4*1024)) {
                return PointerStatus::clean();
              }
            }
            // for other IntToPtrs, ideally, we have alias information, and some
            // alias will have a status. (this comes up with constant IntToPtrs
            // introduced by our pointer encoding)
            return PointerStatus::notdefinedyet();
          }
          default: {
            dbgs() << "unhandled constant expression:\n";
            expr->dump();
            llvm_unreachable("getting status of unhandled constant expression");
          }
        }
      } else {
        // a constant, but not null, a global, or a constant expression.
        dbgs() << "constant pointer of unhandled kind:\n";
        constant->dump();
        llvm_unreachable("getting status of constant pointer of unhandled kind");
      }
    } else {
      // not found in map, and not a constant.
      return PointerStatus::notdefinedyet();
    }
  }

  public:
  MapEqualityResult<const Value*> isEqualTo(const PointerStatuses& other) const {
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
    SmallVector<const Value*, 8> dyn_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> dynnull_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> ndy_ptrs = SmallVector<const Value*, 8>();
    for (auto& pair : map) {
      const Value* ptr = pair.getFirst();
      if (ptr->hasName() && ptr->getName().startswith("__DMS")) {
        // name starts with __DMS, skip it
        continue;
      }
      const PointerStatus& status = pair.getSecond();
      switch (status.kind) {
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
          unk_ptrs.push_back(ptr);
          break;
        case PointerKind::DYNAMIC:
          if (status.dynamic_kind) dyn_ptrs.push_back(ptr);
          else dynnull_ptrs.push_back(ptr);
          break;
        case PointerKind::NOTDEFINEDYET:
          ndy_ptrs.push_back(ptr);
          break;
        default:
          llvm_unreachable("Missing PointerKind case");
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
    out << " and ";
    describePointerList(dyn_ptrs, out, "dyn");
    out << " and ";
    describePointerList(dynnull_ptrs, out, "dynnull");
    out << " and ";
    describePointerList(ndy_ptrs, out, "ndy");
    return out.str();
  }

  /// Merge the given PointerStatuses. If they disagree on any pointer, use
  /// `PointerStatus::merge_with_phi` to combine the statuses.
  /// Recall that any pointer not appearing in the `map` is considered NOTDEFINEDYET.
  ///
  /// `merge_block`: block where the merge is happening. The merged statuses are for this block.
  static PointerStatuses merge(const SmallVector<const PointerStatuses*, 4>& statuses, BasicBlock& merge_block) {
    assert(statuses.size() > 0);
    for (size_t i = 0; i < statuses.size() - 1; i++) {
      assert(statuses[i]->DL == statuses[i+1]->DL);
      assert(statuses[i]->trust_llvm_struct_types == statuses[i+1]->trust_llvm_struct_types);
      assert(&statuses[i]->added_insts == &statuses[i+1]->added_insts);
    }
    PointerStatuses merged(merge_block, statuses[0]->DL, statuses[0]->trust_llvm_struct_types, statuses[0]->added_insts, statuses[0]->pointer_aliases);
    for (size_t i = 0; i < statuses.size(); i++) {
      for (const auto& pair : statuses[i]->map) {
        SmallVector<StatusWithBlock, 4> statuses_for_ptr;
        const Value* ptr = pair.getFirst();
        statuses_for_ptr.push_back(
          StatusWithBlock(pair.getSecond(), &statuses[i]->block)
        );
        bool handled = false;
        for (size_t prev = 0; prev < i; prev++) {
          const auto& it = statuses[prev]->map.find(ptr);
          // if this ptr is in a previous map, we've already handled it
          if (it != statuses[prev]->map.end()) {
            handled = true;
            break;
          }
          // otherwise it's implicitly NOTDEFINEDYET in that map
          statuses_for_ptr.push_back(
            StatusWithBlock(PointerStatus::notdefinedyet(), &statuses[prev]->block)
          );
        }
        if (handled) continue;
        // pointer is not in a previous map, so we need to handle it now
        // get the statuses in the next map(s), if any
        for (size_t next = i + 1; next < statuses.size(); next++) {
          const auto& it = statuses[next]->map.find(ptr);
          PointerStatus status_in_next = (it == statuses[next]->map.end()) ?
            // implicitly NOTDEFINEDYET in next
            PointerStatus::notdefinedyet() :
            // defined in next, get the status
            it->getSecond();
          statuses_for_ptr.push_back(
            StatusWithBlock(status_in_next, &statuses[next]->block)
          );
        }
        // now we have all the statuses, do the merge
        merged.mark_as(ptr, PointerStatus::merge_with_phi(statuses_for_ptr, ptr, &merge_block));
      }
    }
    return merged;
  }
};

template<typename K, typename V, unsigned N>
static MapEqualityResult<K> mapsAreEqual(const SmallDenseMap<K, V, N> &A, const SmallDenseMap<K, V, N> &B) {
  // first: maps of different sizes can never be equal
  if (A.size() != B.size()) return MapEqualityResult<K>::not_same_size();
  // now check that all keys in A are also in B, and map to the same things
  for (const auto &pair : A) {
    const K& key = pair.getFirst();
    const auto& it = B.find(key);
    if (it == B.end()) {
      // key wasn't in B
      return MapEqualityResult<K>::not_equal(key);
    }
    if (it->getSecond() != pair.getSecond()) {
      // maps disagree on what this key maps to
      return MapEqualityResult<K>::not_equal(key);
    }
  }
  // we don't need the reverse check (all keys in B are also in A) because we
  // already checked that A and B have the same number of keys, and all keys in
  // A are also in B
  return MapEqualityResult<K>::is_equal();
}

class DMSAnalysis final {
public:
  struct Settings {
    /// if `true`, then we will assume that, e.g., if we have a CLEAN pointer to
    /// a struct, and derive a pointer to the nth element of that struct, the
    /// resulting pointer is also CLEAN.
    /// This assumption could get us in trouble if the original "pointer to a
    /// struct" was actually a pointer to some smaller object, and was casted to
    /// this pointer type.  E.g., this could happen if we incorrectly cast a
    /// `void*` to a struct pointer in C.
    bool trust_llvm_struct_types;
    /// the `PointerKind` to use for pointers generated by `inttoptr`
    /// instructions, i.e., by casting an integer to a pointer. This can be any
    /// `PointerKind` -- e.g., CLEAN, DIRTY, UNKNOWN, etc.
    PointerKind inttoptr_kind;
    /// if `true`, then the `DMSAnalysis` will modify the in-memory
    /// representation of pointers, using bits 48 and 49 to indicate the
    /// DynamicPointerKind.
    /// This informs dynamic counts, and also allows us to bypass dynamic bounds
    /// checks for pointers where bits 48 and 49 indicate they are CLEAN (or
    /// BLEMISHED16).
    bool do_pointer_encoding;
    /// if `true`, then the `DMSAnalysis` will instrument the code to collect
    /// dynamic counts of pointer kinds. These will be printed at runtime
    /// according to the `dynamic_print_type` setting.
    bool instrument_for_dynamic_counts;
    /// If `instrument_for_dynamic_counts` is true, where should we print the
    /// dynamic counts (at runtime)? (This is ignored if
    /// `instrument_for_dynamic_counts` is false.)
    DynamicResults::PrintType dynamic_print_type;
    /// if `true`, then the `DMSAnalysis` will add SW spatial safety checks
    /// where necessary.
    /// Currently this assumes that dereferencing BLEMISHED16 pointers does not
    /// need a SW check, but all other BLEMISHED pointers need checks.
    bool add_sw_spatial_checks;
  };

  /// Creates and initializes the Analysis but doesn't actually run the analysis.
  DMSAnalysis(Function &F, FunctionAnalysisManager &FAM, Settings settings)
    : F(F), DL(F.getParent()->getDataLayout()),
      loopinfo(FAM.getResult<LoopAnalysis>(F)), pdtree(FAM.getResult<PostDominatorTreeAnalysis>(F)),
      settings(settings),
      RPOT(ReversePostOrderTraversal<BasicBlock *>(&F.getEntryBlock())),
      blocks_in_function(F.getBasicBlockList().size()),
      pointer_encoding_is_complete(false),
      block_states(BlockStates(F, DL, settings.trust_llvm_struct_types, added_insts, pointer_aliases)),
      bounds_infos(BoundsInfos(F, DL, added_insts, pointer_aliases))
  {}

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
    // How many stores have a clean/dirty pointer as address (this doesn't count
    // the data being stored, even if it's a pointer)
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

  /// Runs the analysis and returns the `StaticResults`
  StaticResults run() {
    IterationResult res;
    res.changed = true;

    while (res.changed) {
      res = doIteration(NULL, false);
    }

    if (settings.instrument_for_dynamic_counts) {
      instrument();
    }

    if (settings.add_sw_spatial_checks) {
      addSpatialSafetySWChecks();
    }

    // now that we're done, clean up any instructions we added that
    // ended up not being used. For instance, dynamically computing
    // bounds for a pointer that ended up never needing a bounds check.
    for (BasicBlock& block : F) {
      SimplifyInstructionsInBlock(&block);
    }

    DEBUG_WITH_TYPE("DMS-ir-dump",
      dbgs() << "Final IR after DMS pass:\n";
      F.dump();
    );

    if (verifyFunction(F, &errs())) {
      errs() << "Verify failed for function " << F.getNameOrAsOperand() << " after DMS\n";
      errs() << "Function IR is:\n";
      F.dump();
    }

    verifyGVUsersAreWellFormed(F);

    return res.static_results;
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
  const Settings settings;

  /// we use "reverse post order" in an attempt to process block predecessors
  /// before the blocks themselves. (Of course, this isn't possible to do
  /// perfectly, because of loops.)
  /// We also store this as a class member because constructing it is expensive,
  /// according to the docs in PostOrderIterator.h. We don't want to construct it
  /// every `doIteration()`. However, we do reconstruct it if blocks are added
  /// to the function.
  ReversePostOrderTraversal<BasicBlock*> RPOT;
  /// This is used to help us determine whether the RPOT needs to be recomputed.
  /// We assume that our pass never removes blocks, only adds them.
  /// So, if the number of blocks has increased since last iteration, the RPOT
  /// needs to be recomputed.
  unsigned blocks_in_function;

  /// True if we've already inserted the dynamic instructions that encode/decode
  /// pointers stored in memory.
  bool pointer_encoding_is_complete;

  /// List of instructions which were added for the purpose of computing status
  /// or bounds information. They aren't in the original program (so shouldn't
  /// count for stats purposes). The results of these instructions will be used
  /// only to determine dynamic pointer statuses and/or bounds, if ever; if they
  /// are pointers, they will never be dereferenced.
  ///
  /// We don't necessarily add all the instructions we insert for status/bounds
  /// purposes to this set. But, the intent is that we at minimum add all the
  /// pointer-producing instructions, and all the calls.
  DenseSet<const Instruction*> added_insts;

  /// Map from a pointer to a set of pointers it is known to be exactly equal to
  /// (perhaps bitcasted, or GEP'd with 0 offset, but the pointer value is
  /// equal).
  /// If a status is updated for this pointer at a given program point, the
  /// status of the other pointers at that program point will be updated as
  /// well.
  DenseMap<const Value*, SmallDenseSet<const Value*>> pointer_aliases;

  /// Map from Store instructions which store pointers, to the _original_
  /// pointer being stored (ie, before pointer encoding), and to the status
  /// which was used to compute the pointer encoding for it
  struct OrigPointerAndStatus {
    /// Original pointer being stored (ie, before pointer encoding)
    Value* orig_ptr;
    /// Status which was used to compute the pointer encoding for it
    PointerStatus status;
  };
  DenseMap<const StoreInst*, OrigPointerAndStatus> original_stored_ptrs;

  /// This holds the per-block state for the analysis. One instance of this
  /// class is kept per block in the function.
  class PerBlockState final {
  public:
    PerBlockState(
      BasicBlock& block,
      const DataLayout &DL,
      const bool trust_llvm_struct_types,
      DenseSet<const Instruction*>& added_insts,
      DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
    ) :
      ptrs_beg(PointerStatuses(block, DL, trust_llvm_struct_types, added_insts, pointer_aliases)),
      ptrs_end(PointerStatuses(block, DL, trust_llvm_struct_types, added_insts, pointer_aliases)),
      static_results(StaticResults { 0 }),
      added_insts(added_insts)
    {}

    /// The status of all pointers at the _beginning_ of the block.
    PointerStatuses ptrs_beg;
    /// The status of all pointers at the _end_ of the block.
    PointerStatuses ptrs_end;
    /// The `StaticResults` which we got last time we analyzed this block.
    DMSAnalysis::StaticResults static_results;

    /// Manual overrides of pointer status at the top of this block.
    /// Maps a pointer to its status.
    /// If a pointer isn't in this map, its status will be computed normally.
    SmallDenseMap<const Value*, PointerStatus, 4> status_overrides_at_top;

  private:
    /// Reference to the `added_insts` where we note any instructions added for
    /// bounds purposes. See notes on `added_insts` in `DMSAnalysis`
    DenseSet<const Instruction*>& added_insts;
  };

  /// This holds the per-block states for all blocks
  class BlockStates final {
  private:
    DenseMap<const BasicBlock*, PerBlockState*> block_states;

  public:
    explicit BlockStates(
      Function& F,
      const DataLayout &DL,
      const bool trust_llvm_struct_types,
      DenseSet<const Instruction*>& added_insts,
      DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
    ) :
      DL(DL),
      trust_llvm_struct_types(trust_llvm_struct_types),
      added_insts(added_insts),
      pointer_aliases(pointer_aliases)
    {
      // For now, if any function parameters are pointers,
      // mark them UNKNOWN in the function's entry block
      PerBlockState& entry_block_pbs = lookup(F.getEntryBlock());
      for (const Argument& arg : F.args()) {
        if (arg.getType()->isPointerTy()) {
          // we use PointerStatuses::mark_as() directly. We don't need the
          // DMSAnalysis::mark_as() here because we can't have any aliases yet
          entry_block_pbs.ptrs_beg.mark_as(&arg, PointerStatus::unknown());
        }
      }

      // Mark pointers to global variables (and other global values, e.g.,
      // functions and IFuncs) as CLEAN in the function's entry block.
      // (If the global variable itself is a pointer, it's still implicitly
      // NOTDEFINEDYET.)
      for (const GlobalValue& gv : F.getParent()->global_values()) {
        assert(gv.getType()->isPointerTy());
        // we use PointerStatuses::mark_as() directly. We don't need the
        // DMSAnalysis::mark_as() here because we can't have any aliases yet
        entry_block_pbs.ptrs_beg.mark_as(&gv, PointerStatus::clean());
      }
    }

    /// Get the PerBlockState for the given block, constructing a blank
    /// PerBlockState if needed.
    PerBlockState& lookup(BasicBlock& block) {
      if (block_states.count(&block) > 0) {
        return *block_states[&block];
      } else {
        PerBlockState* pbs = new PerBlockState(
          block,
          DL,
          trust_llvm_struct_types,
          added_insts,
          pointer_aliases
        );
        block_states[&block] = pbs;
        return *pbs;
      }
    }

    ~BlockStates() {
      // clean up the `PerBlockState`s which were created with `new`
      for (auto& pair : block_states) {
        delete pair.getSecond();
      }
    }

  private:
    const DataLayout& DL;
    const bool trust_llvm_struct_types;

    /// Reference to the `added_insts` where we note any instructions added for
    /// bounds purposes. See notes on `added_insts` in `DMSAnalysis`
    DenseSet<const Instruction*>& added_insts;

    /// Reference to the `pointer_aliases` for this function; see notes there
    DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases;
  };

  BlockStates block_states;

  /// Mark the given `ptr` (and all of its aliases, see `pointer_aliases`) as
  /// the given `PointerStatus`, in the given `PointerStatuses`
  void mark_as(PointerStatuses& statuses, const Value* ptr, PointerStatus status) {
    statuses.mark_as(ptr, status);
    for (const Value* alias : pointer_aliases[ptr]) {
      statuses.mark_as(alias, status);
    }
  }

  /// If the `DMSAnalysis` has `do_pointer_encoding`, this maps loaded value
  /// (type `LoadInst`) to its `PointerStatus` at the program point right after
  /// it is loaded (which will be DYNAMIC and contain the loaded value's
  /// `dynamic_kind`). This mapping can't change from iteration to iteration.
  DenseMap<const LoadInst*, PointerStatus> loaded_val_statuses;

  /// For the IntToPtrs in this map, don't count them for stats, and lock the
  /// result's PointerStatus and BoundsInfo to be the same status/boundsinfo as
  /// that of the corresponding Value.
  /// This is used for the IntToPtrs which we insert ourselves as part of
  /// pointer encoding/decoding.
  DenseMap<const IntToPtrInst*, const Value*> inttoptr_status_and_bounds_overrides;

  /// `Load` and `Store` instructions for which we have already inserted bounds
  /// checks. This way, we know which instructions may still need checks during
  /// the next iteration.
  DenseSet<const Instruction*> checked_insts;

  /// Bounds information for all pointers in the function.
  BoundsInfos bounds_infos;

  /// Instruments the code for dynamic counts.
  /// This must only be called after the analysis is complete (pointer statuses
  /// have reached fixpoint).
  void instrument() {
    DynamicResults results(*F.getParent());
    IterationResult res = doIteration(NULL, false);
    assert(!res.changed && "we should have reached fixpoint before calling instrument()");
    doIteration(&results, false);
    results.addPrint(settings.dynamic_print_type);
  }

  /// Adds SW spatial safety checks where necessary.
  /// This must only be called after the analysis is complete (pointer statuses
  /// have reached fixpoint).
  void addSpatialSafetySWChecks() {
    IterationResult res = doIteration(NULL, false);
    assert(!res.changed && "we should have reached fixpoint before calling addSpatialSafetySWChecks()");
    BoundsInfos::module_initialization(*F.getParent(), added_insts, pointer_aliases);
    doIteration(NULL, true);
    // `doIteration` with add_spatial_sw_checks=true will sometimes split basic
    // blocks, which is a problem when it's also trying to iterate over all the
    // blocks in the function. To ensure that we don't miss any blocks that were
    // created or split, we continue iterating until the number of blocks doesn't
    // change.
    while (F.getBasicBlockList().size() > blocks_in_function) {
      doIteration(NULL, true);
    }
  }

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
  IterationResult doIteration(DynamicResults* dynamic_results, const bool add_spatial_sw_checks) {
    StaticResults static_results = { 0 };
    bool changed = false;

    LLVM_DEBUG(
      dbgs() << "DMS: starting an iteration through function " << F.getName();
      if (dynamic_results) dbgs() << ", with instrumentation for dynamic counts";
      if (add_spatial_sw_checks) dbgs() << ", adding spatial sw checks";
      dbgs() << "\n";
    );

    // Recompute the RPOT if necessary. See notes on RPOT
    unsigned new_num_bbs = F.getBasicBlockList().size();
    if (new_num_bbs > blocks_in_function) {
      blocks_in_function = new_num_bbs;
      RPOT = ReversePostOrderTraversal<BasicBlock*>(&F.getEntryBlock());
    }

    // now the main analysis
    for (BasicBlock* block : RPOT) {
      AnalyzeBlockResult res = analyze_block(*block, dynamic_results, add_spatial_sw_checks);
      changed |= res.end_of_block_statuses_changed;
      static_results += res.static_results;
    }

    if (settings.do_pointer_encoding) pointer_encoding_is_complete = true;

    LLVM_DEBUG(
      dbgs() << "DMS: finished an iteration through function " << F.getName()
        << "; total of " << F.size() << " blocks and " << bounds_infos.numTrackedPtrs() << " bounds-tracked ptrs\n";
    );

    return IterationResult { changed, static_results };
  }

  /// Compute the `PointerStatuses` for the top of the given block, based on the
  /// current `PointerStatuses` at the end of the block's predecessors.
  ///
  /// Caller must not call this on a block with no predecessors (e.g., the entry
  /// block).
  PointerStatuses computeTopOfBlockPointerStatuses(BasicBlock &block) {
    assert(block.hasNPredecessorsOrMore(1));
    SmallVector<const PointerStatuses*, 4> pred_statuses;
    for (BasicBlock* pred : predecessors(&block)) {
      const PerBlockState& pred_pbs = block_states.lookup(*pred);
      pred_statuses.push_back(&pred_pbs.ptrs_end);
    }
    assert(pred_statuses.size() == pred_size(&block));
    PointerStatuses ptr_statuses = PointerStatuses::merge(pred_statuses, block);
    // now we handle the manual overrides
    const PerBlockState& pbs = block_states.lookup(block);
    for (auto& pair : pbs.status_overrides_at_top) {
      ptr_statuses.mark_as(pair.getFirst(), pair.getSecond());
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
    PerBlockState& pbs = block_states.lookup(block);

    LLVM_DEBUG(dbgs() << "DMS: analyzing block " << block.getNameOrAsOperand() << "\n");
    DEBUG_WITH_TYPE("DMS-block-previous-state", dbgs() << "DMS:   this block previously had " << pbs.ptrs_beg.describe() << " at beginning and " << pbs.ptrs_end.describe() << " at end\n");

    // this is just so we can report how many new instructions were added
    // during this call to analyze_block at the end
    auto num_added_insts_before_analyze_block = added_insts.size();

    bool isEntryBlock = block.hasNPredecessors(0);  // technically a dead block could also have 0 predecessors, but we don't care what this analysis does with dead blocks. (If you run this pass after optimizations there shouldn't be dead blocks anyway.)

    // The current pointer statuses. As we go through the block, this gets
    // updated; its state at the end of the block will become `pbs.ptrs_end`.
    PointerStatuses ptr_statuses = isEntryBlock ?
      // for the entry block, we already correctly initialized the top-of-block
      // pointer statuses, so just retrieve those and return them
      pbs.ptrs_beg :
      // for all other blocks, compute the top-of-block pointer statuses based
      // on the block's predecessors
      computeTopOfBlockPointerStatuses(block);
    DEBUG_WITH_TYPE("DMS-block-stats", dbgs() << "DMS:   at beginning of block, we have " << ptr_statuses.describe() << "\n");

    if (!isEntryBlock) {
      // Let's check if that's any different from what we had last time
      MapEqualityResult<const Value*> MER = ptr_statuses.isEqualTo(pbs.ptrs_beg);
      if (MER.equal) {
        // no change. Unless we're adding dynamic instrumentation, there's no
        // need to actually analyze this block. Just return the `StaticResults`
        // we got last time.
        LLVM_DEBUG(dbgs() << "DMS:   top-of-block statuses haven't changed\n");
        if (!dynamic_results && !add_spatial_sw_checks) return AnalyzeBlockResult { false, pbs.static_results };
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
        pbs.ptrs_beg = ptr_statuses;
      }
    }

    DEBUG_WITH_TYPE("DMS-inst-processing",
      auto num_new_insts = added_insts.size() - num_added_insts_before_analyze_block;
      if (num_new_insts > 0) dbgs() << "DMS:   " << num_new_insts << " new instructions added in top-of-block processing\n";
    );

    StaticResults static_results = { 0 };

    // Blocks which have been created/inserted during this call to
    // `analyze_block`
    SmallVector<BasicBlock*, 4> new_blocks;

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
      DEBUG_WITH_TYPE("DMS-inst-processing", dbgs() << "DMS:   processing instruction "; inst.dump());
      if (added_insts.count(&inst)) {
        // see notes on added_insts
        DEBUG_WITH_TYPE("DMS-inst-processing", dbgs() << "DMS:     it's an added inst\n");
        continue;
      }
      bool block_done = false;
      // this is just so we can report how many new instructions were added
      // while processing this instruction
      auto num_added_insts_before_processing_this_inst = added_insts.size();
      switch (inst.getOpcode()) {
        case Instruction::Store: {
          StoreInst& store = cast<StoreInst>(inst);
          // first count the address
          Value* addr = store.getPointerOperand();
          COUNT_OP_AS_STATUS(store_addrs, ptr_statuses.getStatus(addr), &inst, "Storing to pointer");
          // insert a bounds check before the store, if necessary
          if (add_spatial_sw_checks && !checked_insts.count(&store)) {
            DEBUG_WITH_TYPE("DMS-inst-processing", dbgs() << "DMS:   inserting a bounds check if necessary\n");
            DMSIRBuilder BeforeStore(&store, DMSIRBuilder::BEFORE, &added_insts);
            const unsigned store_size_bytes = DL.getTypeStoreSize(store.getValueOperand()->getType()).getFixedSize();
            block_done |= maybeAddSpatialSWCheck(addr, ptr_statuses.getStatus(addr), store_size_bytes, BeforeStore, new_blocks);
            checked_insts.insert(&store);
          }
          // now, the pointer used as an address becomes clean
          mark_as(ptr_statuses, addr, PointerStatus::clean());

          // if we're storing a pointer we have some extra work to do.
          Value* storedVal = store.getValueOperand();
          if (storedVal->getType()->isPointerTy()) {
            // we count the stored pointer for stats purposes
            PointerStatus storedVal_status = ptr_statuses.getStatus(storedVal);
            COUNT_OP_AS_STATUS(store_vals, storedVal_status, &inst, "Storing a pointer");
            OrigPointerAndStatus orig_ptr_and_status = original_stored_ptrs.lookup(&store);
            if (settings.add_sw_spatial_checks) {
              bounds_infos.propagate_bounds(store, orig_ptr_and_status.orig_ptr);
            }
            // make sure the stored pointer is masked as necessary
            if (pointer_encoding_is_complete) {
              // if the status of the stored pointer has changed since last
              // iteration, we need to re-encode it using the updated status
              if (orig_ptr_and_status.status == storedVal_status) {
                // nothing to do
              } else {
                LLVM_DEBUG(dbgs() << "DMS:   re-encoding a stored pointer\n");
                // see notes below on the `do_pointer_encoding` case
                DMSIRBuilder Builder(&store, DMSIRBuilder::BEFORE, NULL);
                Value* encoded_ptr = encode_ptr(orig_ptr_and_status.orig_ptr, storedVal_status, Builder);
                store.setOperand(0, encoded_ptr);
                original_stored_ptrs[&store].status = storedVal_status;
                // commented out because we "shouldn't" need to look up the status
                // of the encoded ptr in the future, as it's only used in this
                // one place; and this way we avoid interfering with the fixpoint
                // calculation (making it think some status has "changed")
                //ptr_statuses.mark_as(encoded_ptr, storedVal_status);
              }
            } else if (settings.do_pointer_encoding) {
              // modify the store instruction to store the encoded pointer
              // instead. When we later load this pointer from memory, we will
              // decode it to learn the pointer type and make the pointer valid
              // for use again.
              LLVM_DEBUG(dbgs() << "DMS:   encoding a stored pointer\n");
              DMSIRBuilder Builder(&store, DMSIRBuilder::BEFORE, NULL); // we explicitly don't want to add these to inserted_insts; we still want to track and process them in future iterations
              Value* encoded_ptr = encode_ptr(storedVal, storedVal_status, Builder);
              // store the encoded value instead of the old one
              store.setOperand(0, encoded_ptr);
              original_stored_ptrs[&store] = OrigPointerAndStatus { storedVal, storedVal_status };
              // and also mark the status of the new (encoded) ptr -- it's the same as the
              // status of the original (unencoded) ptr
              ptr_statuses.mark_as(encoded_ptr, storedVal_status);
            }
          }
          break;
        }
        case Instruction::Load: {
          LoadInst& load = cast<LoadInst>(inst);
          Value* ptr = load.getPointerOperand();
          // first count this for static stats
          COUNT_OP_AS_STATUS(load_addrs, ptr_statuses.getStatus(ptr), &inst, "Loading from pointer");
          // insert a bounds check before the load, if necessary
          if (add_spatial_sw_checks && !checked_insts.count(&load)) {
            DEBUG_WITH_TYPE("DMS-inst-processing", dbgs() << "DMS:   inserting a bounds check if necessary\n");
            DMSIRBuilder BeforeLoad(&load, DMSIRBuilder::BEFORE, &added_insts);
            const unsigned load_size_bytes = DL.getTypeStoreSize(load.getType()).getFixedSize();
            block_done |= maybeAddSpatialSWCheck(ptr, ptr_statuses.getStatus(ptr), load_size_bytes, BeforeLoad, new_blocks);
            checked_insts.insert(&load);
          }
          // now, the pointer becomes clean
          mark_as(ptr_statuses, ptr, PointerStatus::clean());

          if (load.getType()->isPointerTy()) {
            // in this case, we loaded a pointer from memory, so we
            // only know its status and bounds dynamically, not statically.
            // See notes above on the Store case.
            if (pointer_encoding_is_complete) {
              // get the status from `loaded_val_statuses`; see notes there
              PointerStatus status = loaded_val_statuses[&load];
              ptr_statuses.mark_as(&load, status);
              if (settings.add_sw_spatial_checks) {
                // bounds info remains valid from iteration to iteration (our
                // fixpoint won't change the bounds info here), so we don't
                // need to change anything now. we computed the bounds info
                // when we did the pointer encoding.
                assert(bounds_infos.is_binfo_present(&load));
              }
            } else if (settings.do_pointer_encoding) {
              // insert the instructions to interpret the encoded pointer
              DMSIRBuilder AfterLoad_NoTrack(&load, DMSIRBuilder::AFTER, NULL); // a builder which explicitly doesn't add its instructions to inserted_insts; we still want to track and process them in future iterations
              auto tuple = decode_ptr(&load, AfterLoad_NoTrack);
              Instruction* decoded_ptr = std::get<0>(tuple);
              PointerStatus loaded_ptr_status = std::get<1>(tuple);
              User* decode_user = std::get<2>(tuple);
              // replace all uses of `load` with the decoded ptr, except of
              // course the `decoded_ptr_use`, which is necessary for actually
              // decoding the loaded pointer
              load.replaceUsesWithIf(
                decoded_ptr,
                [decode_user](Use &U){ return U.getUser() != decode_user; }
              );
              ptr_statuses.mark_as(&load, loaded_ptr_status);
              ptr_statuses.mark_as(decoded_ptr, loaded_ptr_status);
              // store this mapping in `loaded_val_statuses`; see notes there
              loaded_val_statuses[&load] = loaded_ptr_status;
              if (settings.add_sw_spatial_checks) {
                bounds_infos.propagate_bounds(load, decoded_ptr);
              }
            } else {
              // when not `do_pointer_encoding`, we're allowed to pass NULL
              // here. See notes on `PointerStatus::dynamic_kind`
              mark_as(ptr_statuses, &load, PointerStatus::dynamic(NULL));
              if (settings.add_sw_spatial_checks) {
                // bounds info remains valid from iteration to iteration (our
                // fixpoint won't change the bounds info here), so we only need to
                // insert the instructions computing it the first time.
                if (!bounds_infos.is_binfo_present(&load)) {
                  bounds_infos.propagate_bounds(load, &load);
                }
              }
            }
          }
          break;
        }
        case Instruction::Alloca: {
          AllocaInst& alloca = cast<AllocaInst>(inst);
          // result of an alloca is a clean pointer
          mark_as(ptr_statuses, &alloca, PointerStatus::clean());
          if (settings.add_sw_spatial_checks) {
            bounds_infos.propagate_bounds(alloca);
          }
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
          GEPResultClassification grc = classifyGEPResult_cached(
            gep, input_status, DL,
            settings.trust_llvm_struct_types,
            ipr.is_induction_pattern ? &ipr.induction_offset : NULL,
            added_insts
          );
          mark_as(ptr_statuses, &gep, grc.classification);
          // if we added a nonzero constant to a pointer, count that for stats purposes
          if (grc.offset.is_constant && !grc.trustworthy_struct_offset && grc.offset.offset != 0) {
            COUNT_OP_AS_STATUS(pointer_arith_const, input_status, &gep, "GEP on a pointer");
          }
          // update aliasing information
          if (grc.offset.is_constant && !grc.trustworthy_struct_offset && grc.offset.offset == 0) {
            pointer_aliases[&gep].insert(input_ptr);
            pointer_aliases[input_ptr].insert(&gep);
          }
          // update bounds info
          if (settings.add_sw_spatial_checks) {
            bounds_infos.propagate_bounds(gep);
          }
          break;
        }
        case Instruction::BitCast:
        case Instruction::AddrSpaceCast:
        {
          if (inst.getType()->isPointerTy()) {
            const Value* input_ptr = inst.getOperand(0);
            mark_as(ptr_statuses, &inst, ptr_statuses.getStatus(input_ptr));
            pointer_aliases[input_ptr].insert(&inst);
            pointer_aliases[&inst].insert(input_ptr);
            if (settings.add_sw_spatial_checks) {
              bounds_infos.propagate_bounds_id(inst);
            }
          }
          break;
        }
        case Instruction::Select: {
          SelectInst& select = cast<SelectInst>(inst);
          if (select.getType()->isPointerTy()) {
            // output is clean if both inputs are clean; etc
            const PointerStatus true_status = ptr_statuses.getStatus(select.getTrueValue());
            const PointerStatus false_status = ptr_statuses.getStatus(select.getFalseValue());
            DMSIRBuilder Builder(&select, DMSIRBuilder::BEFORE, &added_insts);
            mark_as(ptr_statuses, &select, PointerStatus::merge_direct(true_status, false_status, Builder));
            if (settings.add_sw_spatial_checks) {
              bounds_infos.propagate_bounds(select);
            }
          }
          break;
        }
        case Instruction::PHI: {
          PHINode& phi = cast<PHINode>(inst);
          if (phi.getType()->isPointerTy()) {
            SmallVector<StatusWithBlock, 4> incoming_statuses;
            for (const Use& use : phi.incoming_values()) {
              BasicBlock* bb = phi.getIncomingBlock(use);
              auto& ptr_statuses_end_of_bb = block_states.lookup(*bb).ptrs_end;
              Value* value = use.get();
              incoming_statuses.push_back(StatusWithBlock(
                ptr_statuses_end_of_bb.getStatus(value),
                bb
              ));
            }
            PointerStatus merged_status = PointerStatus::merge_with_phi(incoming_statuses, &phi, &block);
            mark_as(ptr_statuses, &phi, std::move(merged_status));
            if (settings.add_sw_spatial_checks) {
              bounds_infos.propagate_bounds(phi);
            }
          }
          break;
        }
        case Instruction::IntToPtr: {
          IntToPtrInst& inttoptr = cast<IntToPtrInst>(inst);
          auto it = inttoptr_status_and_bounds_overrides.find(&inttoptr);
          if (it != inttoptr_status_and_bounds_overrides.end()) {
            // there's an override in place for this IntToPtr.
            // Ignore it for stats purposes, and copy the status and boundsinfo
            // from the indicated Value.
            // See notes on `inttoptr_status_and_bounds_overrides`.
            PointerStatus status = ptr_statuses.getStatus(it->getSecond());
            assert(status.kind != PointerKind::NOTDEFINEDYET);
            mark_as(ptr_statuses, &inttoptr, status);
            if (settings.add_sw_spatial_checks) {
              bounds_infos.mark_as(&inttoptr, bounds_infos.get_binfo(it->getSecond()));
            }
          } else {
            // no override in place for this IntToPtr.
            // count this for stats, and then mark it as `inttoptr_kind`
            static_results.inttoptrs++;
            if (dynamic_results) {
              incrementGlobalCounter(dynamic_results->inttoptrs, &inst);
            }
            mark_as(ptr_statuses, &inttoptr, PointerStatus::from_kind(settings.inttoptr_kind));
            if (settings.add_sw_spatial_checks) {
              bounds_infos.propagate_bounds(inttoptr, settings.inttoptr_kind);
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
            DMSIRBuilder Builder(&call, DMSIRBuilder::BEFORE, &added_insts);
            IsAllocatingCall IAC = isAllocatingCall(call, Builder);
            if (IAC.is_allocating) {
              // If this is an allocating call (eg, a call to `malloc`), then the
              // returned pointer is CLEAN
              mark_as(ptr_statuses, &call, PointerStatus::clean());
            } else {
              // For now, mark pointers returned from other calls as UNKNOWN
              mark_as(ptr_statuses, &call, PointerStatus::unknown());
            }
            if (settings.add_sw_spatial_checks) {
              bounds_infos.propagate_bounds(call, IAC);
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
          mark_as(ptr_statuses, &inst, PointerStatus::unknown());
          if (settings.add_sw_spatial_checks) {
            bounds_infos.mark_as(&inst, BoundsInfo::unknown());
          }
          break;
        }
        case Instruction::ExtractElement: {
          // same comments apply as for ExtractValue, basically
          mark_as(ptr_statuses, &inst, PointerStatus::unknown());
          if (settings.add_sw_spatial_checks) {
            bounds_infos.mark_as(&inst, BoundsInfo::unknown());
          }
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
      DEBUG_WITH_TYPE("DMS-bounds-info",
        if (inst.getType()->isPointerTy()) {
          dbgs() << "DMS:     bounds info for the produced pointer " << inst.getNameOrAsOperand() << " is " << bounds_infos.get_binfo(&inst).pretty() << "\n";
        }
      );
      DEBUG_WITH_TYPE("DMS-inst-processing",
        if (inst.getType()->isPointerTy()) {
          dbgs() << "DMS:     bounds info for the produced pointer " << inst.getNameOrAsOperand() << " is " << bounds_infos.get_binfo(&inst).pretty() << "\n";
        }
        auto num_new_insts = added_insts.size() - num_added_insts_before_processing_this_inst;
        if (num_new_insts > 0) dbgs() << "DMS:   " << num_new_insts << " new instructions added while processing this instruction\n";
      );
      if (block_done) break;
    }

    // Now that we've processed all the instructions, we have the final
    // statuses of pointers as of the end of the block
    DEBUG_WITH_TYPE("DMS-block-stats", dbgs() << "DMS:   at end of block, we now have " << ptr_statuses.describe() << "\n");
    bool changed = !ptr_statuses.isEqualTo(pbs.ptrs_end).equal;
    if (changed) {
      LLVM_DEBUG(dbgs() << "DMS:   end-of-block pointer statuses have changed\n");
    }
    pbs.ptrs_end = std::move(ptr_statuses);
    pbs.static_results = static_results;

    if (!wellFormed(block)) {
      block.dump();
      llvm_unreachable("block not well-formed at end of analyze_block");
    }

    LLVM_DEBUG(
      auto num_new_insts = added_insts.size() - num_added_insts_before_analyze_block;
      auto num_new_blocks = new_blocks.size();
      dbgs() << "DMS: done analyzing block " << block.getNameOrAsOperand();
      if (num_new_insts > 0) dbgs() << "; " << num_new_insts << " new instructions added";
      if (num_new_blocks > 0) dbgs() << "; " << num_new_blocks << " new blocks added";
      dbgs() << "\n";
      if (num_new_insts > 0) {
        dbgs() << "new block:\n";
        block.dump();
      }
    );

    // If any new blocks were created/inserted during this call to
    // `analyze_block`, let's also analyze the new blocks before returning.
    // This ensures that invariants are maintained, in the sense that other
    // blocks may expect that analysis has been completed for all their
    // predecessors before they are analyzed themselves.
    for (BasicBlock* new_block : new_blocks) {
      AnalyzeBlockResult res = analyze_block(*new_block, dynamic_results, add_spatial_sw_checks);
      changed |= res.end_of_block_statuses_changed;
      static_results += res.static_results;
    }

    return AnalyzeBlockResult { changed, static_results };
  }

  /// If necessary, add a dynamic spatial safety check for the dereference of
  /// `addr`, assuming it has status `status` immediately before being
  /// dereferenced.
  ///
  /// The spatial check is for an operation of size `access_bytes`; for example,
  /// for a 64-bit (8-byte) operation, it ensures that all 8 bytes are in
  /// bounds.
  ///
  /// If dynamic instructions need to be inserted, use `Builder`.
  ///
  /// If we create/insert any new blocks, they will be added to `new_blocks`.
  ///
  /// Currently this assumes that dereferencing BLEMISHED16 pointers does not
  /// need a SW check, but all other BLEMISHED pointers need checks.
  ///
  /// Returns `true` if the current basic block was split and thus is done being
  /// processed. (Note this is _not_ the same as, if a SW check was inserted.)
  bool maybeAddSpatialSWCheck(
    Value* addr,
    const PointerStatus status,
    const unsigned access_bytes,
    DMSIRBuilder& Builder,
    SmallVector<BasicBlock*, 4>& new_blocks
  ) {
    switch (status.kind) {
      case PointerKind::CLEAN:
        // no check required
        return false;
      case PointerKind::BLEMISHED16:
        // no check required
        return false;
      case PointerKind::BLEMISHED32:
      case PointerKind::BLEMISHED64:
      case PointerKind::BLEMISHEDCONST:
      case PointerKind::DIRTY: {
        const BoundsInfo& binfo = bounds_infos.get_binfo(addr);
        return sw_bounds_check(addr, binfo, access_bytes, Builder, new_blocks);
      }
      case PointerKind::UNKNOWN: {
        errs() << "warning: status unknown for " << addr->getNameOrAsOperand() << "; assuming dirty and adding SW bounds check\n";
        const BoundsInfo& binfo = bounds_infos.get_binfo(addr);
        return sw_bounds_check(addr, binfo, access_bytes, Builder, new_blocks);
      }
      case PointerKind::DYNAMIC: {
        const BoundsInfo& binfo = bounds_infos.get_binfo(addr);
        if (binfo.get_kind() == BoundsInfo::STATIC) {
          if (binfo.static_info()->fails(access_bytes)) {
            // we check the dynamic kind, if it is DYN_CLEAN or
            // DYN_BLEMISHED16 then we do nothing, else we SW-fail
            Value* needs_check = Builder.CreateLogicalAnd(
              Builder.CreateICmpNE(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::clean)),
              Builder.CreateICmpNE(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16))
            );
            BasicBlock* failbb = boundsCheckFailBB(addr);
            new_blocks.push_back(failbb);
            BasicBlock* newbb = Builder.insertCondJumpTo(needs_check, failbb);
            new_blocks.push_back(newbb);
            return true;
          } else {
            // do nothing: static bounds check passes, so we don't need to
            // insert instructions to check the dynamic PointerKind
            return false;
          }
        } else {
          BasicBlock* boundscheck = BasicBlock::Create(F.getContext(), "", &F);
          new_blocks.push_back(boundscheck);
          Value* needs_check = Builder.CreateLogicalAnd(
            Builder.CreateICmpNE(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::clean)),
            Builder.CreateICmpNE(status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16))
          );
          BasicBlock* cont = Builder.insertCondJumpTo(needs_check, boundscheck);
          new_blocks.push_back(cont);
          DMSIRBuilder BoundsCheckBuilder(boundscheck, DMSIRBuilder::BEGINNING, &added_insts);
          Instruction* br = BoundsCheckBuilder.CreateBr(cont);
          BoundsCheckBuilder.SetInsertPoint(br);
          sw_bounds_check(addr, binfo, access_bytes, BoundsCheckBuilder, new_blocks);
          assert(wellFormed(*boundscheck));
          return true;
        }
      }
      case PointerKind::NOTDEFINEDYET:
        llvm_unreachable("trying to bounds check on pointer with NOTDEFINEDYET status");
      default:
        llvm_unreachable("Missing PointerKind case");
    }
    llvm_unreachable("Should return before we get here");
  }

  /// Insert a SW bounds check of `ptr` against the bounds information in the
  /// given `BoundsInfo`.
  ///
  /// The spatial check is for an operation of size `access_bytes`; for example,
  /// for a 64-bit (8-byte) operation, it ensures that all 8 bytes are in
  /// bounds.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
  /// that is necessary.
  ///
  /// If we create/insert any new blocks, they will be added to `new_blocks`.
  ///
  /// Returns `true` if the current basic block was split and thus is done being
  /// processed.
  bool sw_bounds_check(
    Value* ptr,
    const BoundsInfo& binfo,
    const unsigned access_bytes,
    DMSIRBuilder& Builder,
    SmallVector<BasicBlock*, 4>& new_blocks
  ) {
    switch (binfo.get_kind()) {
      case BoundsInfo::NOTDEFINEDYET:
        llvm_unreachable("Can't sw_bounds_check with NOTDEFINEDYET bounds");
      case BoundsInfo::UNKNOWN:
        errs() << "warning: bounds info unknown for " << ptr->getNameOrAsOperand() << " even though it needs a bounds check. Unsafely omitting the bounds check.\n";
        return false;
      case BoundsInfo::INFINITE:
        // bounds check passes by default
        return false;
      case BoundsInfo::STATIC:
        sw_bounds_check(ptr, *binfo.static_info(), access_bytes, Builder);
        return false;
      case BoundsInfo::DYNAMIC:
      case BoundsInfo::DYNAMIC_MERGED:
        sw_bounds_check(ptr, *binfo.dynamic_info(), access_bytes, Builder, new_blocks);
        return true;
      default:
        llvm_unreachable("Missing BoundsInfo.kind case");
    }
  }

  /// Insert a SW bounds check of `ptr` against the bounds information in the
  /// given `StaticBoundsInfo`.
  ///
  /// The spatial check is for an operation of size `access_bytes`; for example,
  /// for a 64-bit (8-byte) operation, it ensures that all 8 bytes are in
  /// bounds.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
  /// that is necessary.
  void sw_bounds_check(
    Value* ptr,
    const BoundsInfo::StaticBoundsInfo& binfo,
    const unsigned access_bytes,
    DMSIRBuilder& Builder
  ) {
    if (binfo.fails(access_bytes)) {
      call_dms_boundscheckfail(ptr, Builder);
    }
    assert(wellFormed(*Builder.GetInsertBlock()));
  }

  /// Insert a SW bounds check of `ptr` against the bounds information in the
  /// given `DynamicBoundsInfo`.
  ///
  /// The spatial check is for an operation of size `access_bytes`; for example,
  /// for a 64-bit (8-byte) operation, it ensures that all 8 bytes are in
  /// bounds.
  ///
  /// `Builder` is the DMSIRBuilder to use to insert dynamic instructions, if
  /// that is necessary. To be safe, you should assume `Builder` is invalidated
  /// when this function returns.
  ///
  /// If we create/insert any new blocks, they will be added to `new_blocks`.
  void sw_bounds_check(
    Value* ptr,
    const BoundsInfo::DynamicBoundsInfo& binfo,
    const unsigned access_bytes,
    DMSIRBuilder& Builder,
    SmallVector<BasicBlock*, 4>& new_blocks
  ) {
    Value* access_begin = Builder.castToCharStar(ptr);
    Value* access_end = Builder.CreateGEP(Builder.getInt8Ty(), access_begin, {Builder.getInt64(access_bytes - 1)});
    Value* Fail = Builder.CreateLogicalOr(
      Builder.CreateICmpULT(access_begin, binfo.getBase().as_llvm_value(Builder)),
      Builder.CreateICmpUGT(access_end, binfo.getMax().as_llvm_value(Builder))
    );
    BasicBlock* failbb = boundsCheckFailBB(ptr);
    new_blocks.push_back(failbb);
    BasicBlock* newbb = Builder.insertCondJumpTo(Fail, failbb);
    new_blocks.push_back(newbb);
  }

  /// Get a BasicBlock containing code indicating a bounds-check failure for `ptr`.
  /// We can dynamically jump to this block to report a bounds-check failure.
  BasicBlock* boundsCheckFailBB(Value* ptr) {
    BasicBlock* bb = BasicBlock::Create(F.getContext(), "", &F);
    DMSIRBuilder Builder(bb, DMSIRBuilder::BEGINNING, &added_insts);
    call_dms_boundscheckfail(ptr, Builder);
    Builder.CreateUnreachable();
    assert(wellFormed(*bb));
    return bb;
  }

  /// Create and return the "encoded" version of `ptr`, assuming it has the
  /// given `status`.
  /// If dynamic instructions need to be inserted, use `Builder`.
  ///
  /// Specifically, our encoding uses bits 48-49 to indicate the PointerKind of
  /// the pointer, interpreted per the DynamicPointerKind enum.
  /// (We assume all pointers are userspace pointers, so 48-49 should be 0 for
  /// valid pointers.)
  Value* encode_ptr(Value* ptr, const PointerStatus& status, DMSIRBuilder& Builder) {
    // create `new_storedVal` which has the appropriate bits set
    Value* mask = status.to_dynamic_kind_mask(F.getContext());
    Value* ptr_as_int = Builder.CreatePtrToInt(ptr, Builder.getInt64Ty());
    Value* masked_int = Builder.CreateOr(ptr_as_int, mask);
    Value* encoded_ptr = Builder.CreateIntToPtr(masked_int, ptr->getType());
    // create a status and bounds override for the `encoded_ptr`, so
    // this relationship is preserved even in future passes. It will
    // always have the same status and boundsinfo as `ptr`.
    if (IntToPtrInst* encoded_ptr_inttoptr = dyn_cast<IntToPtrInst>(encoded_ptr)) {
      inttoptr_status_and_bounds_overrides[encoded_ptr_inttoptr] = ptr;
    }
    // update aliasing information
    // TODO: can this supercede the inttoptr_status_and_bounds_overrides?
    pointer_aliases[ptr].insert(encoded_ptr);
    pointer_aliases[encoded_ptr].insert(ptr);
    return encoded_ptr;
  }

  /// Given an encoded pointer, "decode" it, returning the original pointer
  /// value and its status.
  /// If dynamic instructions need to be inserted, use `Builder`.
  ///
  /// This function also returns the `User` of the `encoded_ptr` which is
  /// necessary for its decoding. (so that you know not to tamper with that
  /// `User`.)
  ///
  /// Specifically, we check bits 48-49 to learn the pointer type, then clear
  /// them so the pointer is valid for use. See notes on `encode_ptr`.
  std::tuple<Instruction*, PointerStatus, User*> decode_ptr(Value* encoded_ptr, DMSIRBuilder& Builder) {
    Value* encoded_ptr_as_int = Builder.CreatePtrToInt(encoded_ptr, Builder.getInt64Ty());
    Value* dynamic_kind = Builder.CreateAnd(encoded_ptr_as_int, DynamicKindMasks::dynamic_kind_mask);
    Value* decoded_ptr_as_int = Builder.CreateAnd(encoded_ptr_as_int, ~DynamicKindMasks::dynamic_kind_mask);
    Value* decoded_ptr_as_ptr = Builder.CreateIntToPtr(decoded_ptr_as_int, encoded_ptr->getType());
    PointerStatus status = PointerStatus::dynamic(dynamic_kind);

    // create a status and bounds override for the `IntToPtr` which we just
    // inserted: it should always have the same dynamic status and bounds as the
    // `encoded_ptr`. (Caller is responsible for the status and bounds of the
    // `encoded_ptr` itself.)
    if (IntToPtrInst* decoded_ptr_inttoptr = dyn_cast<IntToPtrInst>(decoded_ptr_as_ptr)) {
      inttoptr_status_and_bounds_overrides[decoded_ptr_inttoptr] = encoded_ptr;
    } else {
      LLVM_DEBUG(
        dbgs() << "The following decoded pointer is not an inttoptr instruction:\n";
        decoded_ptr_as_ptr->dump();
      );
    }
    // update aliasing information
    // TODO: can this supercede the inttoptr_status_and_bounds_overrides?
    pointer_aliases[encoded_ptr].insert(decoded_ptr_as_ptr);
    pointer_aliases[decoded_ptr_as_ptr].insert(encoded_ptr);

    return std::make_tuple(
      cast<Instruction>(decoded_ptr_as_ptr),
      status,
      cast<User>(encoded_ptr_as_int)
    );
  }

  // Inject an instruction sequence to increment the given global counter, right
  // before the given instruction
  void incrementGlobalCounter(Constant* GlobalCounter, Instruction* BeforeInst) {
    DMSIRBuilder Builder(BeforeInst, DMSIRBuilder::BEFORE, &added_insts);
    Type* i64ty = Builder.getInt64Ty();
    LoadInst* loaded = Builder.CreateLoad(i64ty, GlobalCounter);
    Value* incremented = Builder.CreateAdd(Builder.getInt64(1), loaded);
    Builder.CreateStore(incremented, GlobalCounter);
  }

  // Inject an instruction sequence to increment the appropriate global counter
  // based on the `PointerStatus`. This is to be used when (and only when) the
  // kind is `DYNAMIC`.
  void incrementGlobalCounterForDynKind(DynamicCounts& dyn_counts, PointerStatus& status, Instruction* BeforeInst) {
    assert(status.kind == PointerKind::DYNAMIC);
    assert(status.dynamic_kind != NULL);
    DMSIRBuilder Builder(BeforeInst, DMSIRBuilder::BEFORE, &added_insts);
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
};

PreservedAnalyses StaticDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DMSAnalysis::Settings settings;
  settings.trust_llvm_struct_types = true;
  settings.inttoptr_kind = PointerKind::CLEAN;
  settings.do_pointer_encoding = false;
  settings.instrument_for_dynamic_counts = false;
  settings.add_sw_spatial_checks = false;

  DMSAnalysis analysis = DMSAnalysis(F, FAM, settings);
  DMSAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // StaticDMSPass only analyzes the IR and doesn't make any changes, so all
  // analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses ParanoidStaticDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DMSAnalysis::Settings settings;
  settings.trust_llvm_struct_types = false;
  settings.inttoptr_kind = PointerKind::DIRTY;
  settings.do_pointer_encoding = false;
  settings.instrument_for_dynamic_counts = false;
  settings.add_sw_spatial_checks = false;

  DMSAnalysis analysis = DMSAnalysis(F, FAM, settings);
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

  DMSAnalysis::Settings settings;
  settings.trust_llvm_struct_types = true;
  settings.inttoptr_kind = PointerKind::CLEAN;
  settings.do_pointer_encoding = true;
  settings.instrument_for_dynamic_counts = true;
  settings.dynamic_print_type = DynamicResults::PrintType::TOFILE;
  settings.add_sw_spatial_checks = false;

  DMSAnalysis analysis = DMSAnalysis(F, FAM, settings);
  analysis.run();

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

  DMSAnalysis::Settings settings;
  settings.trust_llvm_struct_types = true;
  settings.inttoptr_kind = PointerKind::CLEAN;
  settings.do_pointer_encoding = true;
  settings.instrument_for_dynamic_counts = true;
  settings.dynamic_print_type = DynamicResults::PrintType::STDOUT;
  settings.add_sw_spatial_checks = false;

  DMSAnalysis analysis = DMSAnalysis(F, FAM, settings);
  analysis.run();

  // For now we conservatively just tell LLVM that no analyses are preserved.
  // It seems that many existing LLVM passes also just use
  // PreservedAnalyses::none() when they make any change, so we assume this is
  // reasonable.
  return PreservedAnalyses::none();
}

PreservedAnalyses BoundsChecksDMSPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // Don't do any analysis or instrumentation on the special function __DMS_bounds_initialization
  if (F.getName() == "__DMS_bounds_initialization") {
    return PreservedAnalyses::all();
  }

  DMSAnalysis::Settings settings;
  settings.trust_llvm_struct_types = true;
  settings.inttoptr_kind = PointerKind::CLEAN;
  settings.do_pointer_encoding = true;
  settings.instrument_for_dynamic_counts = false;
  settings.add_sw_spatial_checks = true;

  DMSAnalysis analysis = DMSAnalysis(F, FAM, settings);
  analysis.run();

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
  GetElementPtrInst &gep,
  const PointerStatus input_status,
  const DataLayout &DL,
  const bool trust_llvm_struct_types,
  const APInt* override_constant_offset,
  DenseSet<const Instruction*>& added_insts
) {
  assert(input_status.kind != PointerKind::NOTDEFINEDYET && "Shouldn't call classifyGEPResult() with NOTDEFINEDYET input_ptr");
  GEPResultClassification grc;
  if (override_constant_offset == NULL) {
    grc.offset = computeGEPOffset(gep, DL);
  } else {
    grc.offset.is_constant = true;
    grc.offset.offset = *override_constant_offset;
  }

  if (gep.hasAllZeroIndices()) {
    // result of a GEP with all zeroes as indices, is the same as the input pointer.
    assert(grc.offset.is_constant && grc.offset.offset == zero && "If all indices are constant 0, then the total offset should be constant 0");
    grc.classification = input_status;
    grc.trustworthy_struct_offset = false;
    return grc;
  }
  if (trust_llvm_struct_types && areAllIndicesTrustworthy(gep)) {
    // nonzero offset, but "trustworthy" offset.
    assert(grc.offset.is_constant);
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
          DMSIRBuilder Builder(&gep, DMSIRBuilder::BEFORE, &added_insts);
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
  if (grc.offset.is_constant) {
    switch (input_status.kind) {
      case PointerKind::CLEAN: {
        // This GEP adds a constant but nonzero amount to a CLEAN
        // pointer. The result is some flavor of BLEMISHED depending
        // on how far the pointer arithmetic goes.
        if (grc.offset.offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          grc.classification = PointerStatus::blemished16();
          return grc;
        } else if (grc.offset.offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (grc.offset.offset.ule(APInt(/* bits = */ 64, /* val = */ 64))) {
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
        if (grc.offset.offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          // Conservatively, the total offset can't exceed 32
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (grc.offset.offset.ule(APInt(/* bits = */ 64, /* val = */ 48))) {
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
        if (grc.offset.offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
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
          DMSIRBuilder Builder(&gep, DMSIRBuilder::BEFORE, &added_insts);
          Value* is_clean = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::clean));
          Value* is_blem16 = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16));
          Value* is_blemother = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished_other));
          Value* dynamic_kind = Builder.getInt64(DynamicKindMasks::dirty);
          dynamic_kind = Builder.CreateSelect(
            is_clean,
            (grc.offset.offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) ?
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

/// Used only for `classifyGEPResult_cached`; see notes there
struct GEPResultCacheKey {
  GetElementPtrInst* gep;
  PointerStatus input_status;
  bool trust_llvm_struct_types;
  const APInt* override_constant_offset;

  GEPResultCacheKey(
    GetElementPtrInst* gep,
    PointerStatus input_status,
    bool trust_llvm_struct_types,
    const APInt* override_constant_offset
  ) : gep(gep), input_status(input_status), trust_llvm_struct_types(trust_llvm_struct_types), override_constant_offset(override_constant_offset)
  {}

  bool operator==(const GEPResultCacheKey& other) const {
    return
      gep == other.gep &&
      input_status == other.input_status &&
      trust_llvm_struct_types == other.trust_llvm_struct_types &&
      override_constant_offset == other.override_constant_offset;
  }
  bool operator!=(const GEPResultCacheKey& other) const {
    return !(*this == other);
  }
};

// it seems this is required in order for GEPResultCacheKey to be a key type in
// a DenseMap
namespace llvm {
template<> struct DenseMapInfo<GEPResultCacheKey> {
  static inline GEPResultCacheKey getEmptyKey() {
    return GEPResultCacheKey(NULL, PointerStatus::notdefinedyet(), true, NULL);
  }
  static inline GEPResultCacheKey getTombstoneKey() {
    return GEPResultCacheKey(
      DenseMapInfo<GetElementPtrInst*>::getTombstoneKey(),
      PointerStatus::notdefinedyet(),
      true,
      DenseMapInfo<const APInt*>::getTombstoneKey()
    );
  }
  static unsigned getHashValue(const GEPResultCacheKey &Val) {
    return
      DenseMapInfo<GetElementPtrInst*>::getHashValue(Val.gep) ^
      Val.input_status.kind ^
      DenseMapInfo<const Value*>::getHashValue(Val.input_status.dynamic_kind) ^
      (Val.trust_llvm_struct_types ? 0 : -1) ^
      DenseMapInfo<const APInt*>::getHashValue(Val.override_constant_offset);
  }
  static bool isEqual(const GEPResultCacheKey &LHS, const GEPResultCacheKey &RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

/// Same as `classifyGEPResult`, but caches its results. If you call this with
/// the same arguments multiple times, you'll get the same result back.
/// (Critically, this _won't_ insert dynamic instructions on subsequent calls
/// with the same arguments, even if the first call required inserting dynamic
/// instructions.)
static GEPResultClassification classifyGEPResult_cached(
  GetElementPtrInst& gep,
  const PointerStatus input_status,
  const DataLayout &DL,
  const bool trust_llvm_struct_types,
  const APInt* override_constant_offset,
  DenseSet<const Instruction*>& added_insts
) {
  static DenseMap<GEPResultCacheKey, GEPResultClassification> cache;
  GEPResultCacheKey key(&gep, input_status, trust_llvm_struct_types, override_constant_offset);
  if (cache.count(key) > 0) return cache[key];
  GEPResultClassification res = classifyGEPResult(gep, input_status, DL, trust_llvm_struct_types, override_constant_offset, added_insts);
  cache[key] = res;
  return res;
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
  DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "Analyzing the following gep:\n");
  DEBUG_WITH_TYPE("DMS-trustworthy-indices", gep.dump());
  Type* current_ty = gep.getPointerOperandType();
  SmallVector<Constant*, 8> seen_indices;
  for (const Use& idx : gep.indices()) {
    if (!current_ty) {
      DEBUG_WITH_TYPE("DMS-trustworthy-indices", dbgs() << "current_ty is null - probably getIndexedType() returned null\n");
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
