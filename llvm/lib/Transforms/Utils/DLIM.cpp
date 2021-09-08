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

static const APInt zero = APInt(/* bits = */ 64, /* val = */ 0);

/// Return type for `isOffsetAnInductionPattern`.
///
/// If the offset of the given GEP is an induction pattern, then
/// `is_induction_pattern` will be `true`; the GEP has effectively the offset
/// `initial_offset` during the first loop iteration, and the offset is
/// incremented by `induction_offset` each subsequent loop iteration.
/// Offsets are in bytes.
///
/// If the offset is not an induction pattern, then `is_induction_pattern` will
/// be `false`, and the other fields are undefined.
struct InductionPatternResult {
  bool is_induction_pattern;
  APInt induction_offset;
  APInt initial_offset;
};
static InductionPatternResult no_induction_pattern = { false, zero, zero };
/// Is the offset of the given GEP an induction pattern?
/// This is looking for a pretty specific pattern for GEPs inside loops, which
/// we can optimize checks for.
static InductionPatternResult isOffsetAnInductionPattern(const GetElementPtrInst &gep, const DataLayout &DL, const LoopInfo &loopinfo, const PostDominatorTree &pdtree);

/// Return type for `isInductionVar`.
///
/// If the given `val` is an induction variable, then `is_induction_var` will be
/// `true`; `val` is equal to `initial_val` on the first loop iteration, and
/// is incremented by `induction_increment` each iteration.
///
/// If `val` is not an induction variable, then `is_induction_var` will be
/// `false`, and the other fields are undefined.
struct InductionVarResult {
  bool is_induction_var;
  APInt induction_increment;
  APInt initial_val;
};
static InductionVarResult no_induction_var = { false, zero, zero };
/// Is the given `val` an induction variable?
/// Here, "induction variable" is narrowly defined as:
///     a PHI between a constant (initial value) and a variable (induction)
///     equal to itself plus or minus a constant
static InductionVarResult isInductionVar(const Value* val);

/// Return type for `isValuePlusConstant`.
///
/// If the given `val` is equal to another `Value` plus a constant, then `valid`
/// will be `true`; the given `val` is equal to `value` plus `constant`.
///
/// If the given `val` is not equal to another `Value` plus a constant, then
/// `valid` will be `false`, and the other fields are undefined.
struct ValPlusConstantResult {
  bool valid;
  const Value* value;
  APInt constant;
};
static ValPlusConstantResult not_a_val_plus_constant = { false, NULL, zero };
/// Is the given `val` defined as some other `Value` plus/minus a constant?
static ValPlusConstantResult isValuePlusConstant(const Value* val);

template <typename K, typename V, unsigned N> static bool mapsAreEqual(const SmallDenseMap<K, V, N> &A, const SmallDenseMap<K, V, N> &B);
static void describePointerList(const SmallVector<const Value*, 8>& ptrs, std::ostringstream& out, StringRef desc);
static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep);
static bool isAllocatingCall(const CallBase &call);
static bool shouldCountCallForStatsPurposes(const CallBase &call);
static Constant* createGlobalConstStr(Module* mod, const char* global_name, const char* str);
static std::string regexSubAll(const Regex &R, const StringRef Repl, const StringRef String);

class PointerKind {
public:
  enum Kind {
    // As of this writing, the operations producing UNKNOWN are: returning a
    // pointer from a call; or receiving a pointer as a function parameter
    UNKNOWN = 0,
    // CLEAN means "not modified since last allocated or dereferenced", or for
    // some other reason we know it is in-bounds
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
    // BLEMISHEDCONST means "incremented/decremented by some compile-time-constant
    // number of bytes from a clean pointer".
    // We'll make some effort to keep BLEMISHED16 / BLEMISHED32 / BLEMISHED64
    // pointers out of this bucket (leaving this bucket just for constants greater
    // than 64, or negative constants), but if we can't determine which bucket it
    // belongs in, it conservatively goes here.
    BLEMISHEDCONST,
    // DIRTY means "may have been incremented/decremented by a
    // non-compile-time-constant amount since last allocated or dereferenced"
    DIRTY,
    // DYNAMIC means that we don't know the kind statically, but the kind is
    // stored in an LLVM variable. Currently, the only operation producing DYNAMIC
    // is loading a pointer from memory. See `PointerStatus`.
    DYNAMIC,
    // NOTDEFINEDYET means that the pointer has not been defined yet at this program
    // point (at least, to our current knowledge). All pointers are (effectively)
    // initialized to NOTDEFINEDYET at the beginning of the fixpoint analysis, and
    // as we iterate we gradually refine this.
    NOTDEFINEDYET,
  };

  constexpr PointerKind(Kind kind) : kind(kind) { }
  PointerKind() : kind(UNKNOWN) {}

  bool operator==(PointerKind& other) {
    return kind == other.kind;
  }
  bool operator!=(PointerKind& other) {
    return kind != other.kind;
  }

  /// Enable switch() on a `PointerKind` with the expected syntax
  operator Kind() const { return kind; }
  /// Disable if(kind) where `kind` is a `PointerKind`
  explicit operator bool() = delete;

  /// Merge two `PointerKind`s.
  /// For the purposes of this function, the ordering is
  /// DIRTY < UNKNOWN < BLEMISHEDCONST < BLEMISHED64 < BLEMISHED32 < BLEMISHED16 < CLEAN,
  /// and the merge returns the least element.
  /// NOTDEFINEDYET has the property where the merger of x and NOTDEFINEDYET is x
  /// (for all x) - for instance, if we are at a join point in the CFG where the
  /// pointer is x status on one incoming branch and not defined on the other,
  /// the pointer can have x status going forward.
  static PointerKind merge(const PointerKind a, const PointerKind b) {
    if (a == DYNAMIC || b == DYNAMIC) {
      llvm_unreachable("Can't PointerKind::merge a DYNAMIC; use PointerStatus::merge instead");
    } else if (a == NOTDEFINEDYET) {
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
      llvm_unreachable("Missing case in merge function");
    }
  }

  private:
  Kind kind;
};

typedef enum DynamicPointerKind {
  /// Same as PointerKind::CLEAN
  DYN_CLEAN = 3,
  /// Same as PointerKind::BLEMISHED16
  DYN_BLEMISHED16 = 1,
  /// Any BLEMISHED other than BLEMISHED16
  DYN_BLEMISHEDOTHER = 2,
  /// Anything else is conservatively considered DIRTY
  DYN_DIRTY = 0,
} DynamicPointerKind;

static const uint64_t dirty_mask = ((uint64_t)DYN_DIRTY) << 48;
static const uint64_t blemished16_mask = ((uint64_t)DYN_BLEMISHED16) << 48;
static const uint64_t blemished_other_mask = ((uint64_t)DYN_BLEMISHEDOTHER) << 48;
static const uint64_t clean_mask = ((uint64_t)DYN_CLEAN) << 48;
static const uint64_t dynamic_kind_mask = ((uint64_t)0b11) << 48;

/// If C++ had ADTs, PointerKind::DYNAMIC would carry an LLVM Value*.  (And maybe
/// BLEMISHED would carry an int indicating exactly how blemished.)  Instead,
/// we have this.
struct PointerStatus {
  /// the PointerKind
  PointerKind kind;
  /// Only for PointerKind::DYNAMIC, this holds the dynamic kind. This will be a
  /// Value of type i64, where all bits are zeroes except possibly bits 48 and
  /// 49, whose value together indicate the dynamic kind according to
  /// DynamicPointerKind (above).
  ///
  /// This field is undefined if `kind` is not `DYNAMIC`.
  ///
  /// During non-instrumenting iterations, we don't insert new dynamic
  /// instructions and thus can't create or check `dynamic_kind`s.
  /// During these iterations, `dynamic_kind` must be NULL.
  /// During iterations where `pointer_encoding` is `true`, it must not be NULL.
  Value* dynamic_kind;

  static PointerStatus unknown() { return { PointerKind::UNKNOWN, NULL }; }
  static PointerStatus clean() { return { PointerKind::CLEAN, NULL }; }
  static PointerStatus blemished16() { return { PointerKind::BLEMISHED16, NULL }; }
  static PointerStatus blemished32() { return { PointerKind::BLEMISHED32, NULL }; }
  static PointerStatus blemished64() { return { PointerKind::BLEMISHED64, NULL }; }
  static PointerStatus blemishedconst() { return { PointerKind::BLEMISHEDCONST, NULL }; }
  static PointerStatus dirty() { return { PointerKind::DIRTY, NULL }; }
  static PointerStatus notdefinedyet() { return { PointerKind::NOTDEFINEDYET, NULL }; }
  static PointerStatus dynamic(Value* dynamic_kind) { return { PointerKind::DYNAMIC, dynamic_kind }; }

  /// Merge two `PointerStatus`es.
  /// See comments on PointerKind::merge.
  ///
  /// `insertion_pt`: If we need to insert new dynamic instructions to handle
  /// a dynamic merge, insert them before this Instruction.
  /// We will only potentially need to do this if at least one of the statuses
  /// is DYNAMIC with a non-null `dynamic_kind`. If neither of the statuses is
  /// DYNAMIC with a non-null `dynamic_kind`, then this parameter is ignored
  /// (and may be NULL).
  static PointerStatus merge(const PointerStatus a, const PointerStatus b, Instruction* insertion_pt) {
    if (a.kind == PointerKind::DYNAMIC && b.kind == PointerKind::DYNAMIC) {
      return { PointerKind::DYNAMIC, merge_two_dynamic(a.dynamic_kind, b.dynamic_kind, insertion_pt) };
    } else if (a.kind == PointerKind::DYNAMIC) {
      return merge_static_dynamic(b.kind, a.dynamic_kind, insertion_pt);
    } else if (b.kind == PointerKind::DYNAMIC) {
      return merge_static_dynamic(a.kind, b.dynamic_kind, insertion_pt);
    } else {
      return { PointerKind::merge(a.kind, b.kind), NULL };
    }
  }

  /// `Builder`: the `IRBuilder` to use to insert dynamic instructions/values as
  /// necessary
  Value* to_dynamic_kind_mask(IRBuilder<>& Builder) const {
    switch (kind) {
      case PointerKind::CLEAN:
        return Builder.getInt64(clean_mask);
        break;
      case PointerKind::BLEMISHED16:
        return Builder.getInt64(blemished16_mask);
        break;
      case PointerKind::BLEMISHED32:
      case PointerKind::BLEMISHED64:
      case PointerKind::BLEMISHEDCONST:
        return Builder.getInt64(blemished_other_mask);
        break;
      case PointerKind::DIRTY:
        return Builder.getInt64(dirty_mask);
        break;
      case PointerKind::UNKNOWN:
        // for now we just mark UNKNOWN pointers as dirty when storing them
        return Builder.getInt64(dirty_mask);
        break;
      case PointerKind::DYNAMIC:
        return dynamic_kind;
        break;
      case PointerKind::NOTDEFINEDYET:
        llvm_unreachable("Shouldn't call to_dynamic_kind_mask on a NOTDEFINEDYET");
        break;
      default:
        llvm_unreachable("PointerKind case not handled");
    }
  }

  private:
  /// Merge a static `PointerKind` and a `dynamic_kind`.
  /// See comments on PointerStatus::merge.
  static PointerStatus merge_static_dynamic(const PointerKind static_kind, Value* dynamic_kind, Instruction* insertion_pt) {
    if (dynamic_kind == NULL) return PointerStatus::dynamic(NULL);
    assert(insertion_pt && "To merge with a non-null `dynamic_kind`, insertion_pt must not be NULL");
    IRBuilder<> Builder(insertion_pt);
    Value* merged_dynamic_kind;
    switch (static_kind) {
      case PointerKind::NOTDEFINEDYET:
        // As in PointerKind::merge, merging x with NOTDEFINEDYET is always x
        merged_dynamic_kind = dynamic_kind;
        break;
      case PointerKind::CLEAN:
        // For all x, merging CLEAN with x results in x
        merged_dynamic_kind = dynamic_kind;
        break;
      case PointerKind::BLEMISHED16:
        // merging BLEMISHED16 with DYN_CLEAN is DYN_BLEMISHED16.
        // merging BLEMISHED16 with any other x results in x.
        merged_dynamic_kind = Builder.CreateSelect(
          Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(clean_mask)),
          Builder.getInt64(blemished16_mask),
          dynamic_kind
        );
        break;
      case PointerKind::BLEMISHED32:
      case PointerKind::BLEMISHED64:
      case PointerKind::BLEMISHEDCONST:
        // merging any of these with DYN_CLEAN, DYN_BLEMISHED16, or
        // DYN_BLEMISHEDOTHER results in DYN_BLEMISHEDOTHER.
        // merging any of these with DYN_DIRTY results in DYN_DIRTY.
        merged_dynamic_kind = Builder.CreateSelect(
          Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(dirty_mask)),
          Builder.getInt64(dirty_mask),
          Builder.getInt64(blemished_other_mask)
        );
        break;
      case PointerKind::DIRTY:
      case PointerKind::UNKNOWN:
        // merging anything with DIRTY or UNKNOWN results in DYN_DIRTY
        merged_dynamic_kind = Builder.getInt64(dirty_mask);
        break;
      case PointerKind::DYNAMIC:
        llvm_unreachable("merge_static_dynamic: expected a static PointerKind");
      default:
        llvm_unreachable("Missing PointerKind case");
    }
    return PointerStatus::dynamic(merged_dynamic_kind);
  }

  /// Merge two `dynamic_kind`s.
  /// See comments on PointerStatus::merge.
  static Value* merge_two_dynamic(Value* dynamic_kind_a, Value* dynamic_kind_b, Instruction* insertion_pt) {
    if (dynamic_kind_a == NULL) return dynamic_kind_b;
    if (dynamic_kind_b == NULL) return dynamic_kind_a;
    if (dynamic_kind_a == dynamic_kind_b) return dynamic_kind_a;
    // at this point we'll have to do a true dynamic merge.
    // we'll insert dynamic instructions intended to do this (pseudocode):
    //   if (a is dirty or b is dirty) merged_dynamic_kind = dirty
    //   else if (a is blemother or b is blemother) merged_dynamic_kind = blemother
    //   else if (a is blem16 or b is blem16) merged_dynamic_kind = blem16
    //     else merged_dynamic_kind = clean
    // in terms of selects this is:
    //   merged_dynamic_kind =
    //     (a is dirty or b is dirty) ? dirty :
    //     (a is blemother or b is blemother) ? blemother :
    //     (a is blem16 or b is blem16) ? blem16 :
    //     clean
    assert(insertion_pt && "To merge with a non-null `dynamic_kind`, insertion_pt must not be NULL");
    IRBuilder<> Builder(insertion_pt);
    Value* dirty = Builder.getInt64(dirty_mask);
    Value* blemother = Builder.getInt64(blemished_other_mask);
    Value* blem16 = Builder.getInt64(blemished16_mask);
    Value* clean = Builder.getInt64(clean_mask);
    Value* either_is_dirty = Builder.CreateLogicalOr(
      Builder.CreateICmpEQ(dynamic_kind_a, dirty),
      Builder.CreateICmpEQ(dynamic_kind_b, dirty)
    );
    Value* either_is_blemother = Builder.CreateLogicalOr(
      Builder.CreateICmpEQ(dynamic_kind_a, blemother),
      Builder.CreateICmpEQ(dynamic_kind_b, blemother)
    );
    Value* either_is_blem16 = Builder.CreateLogicalOr(
      Builder.CreateICmpEQ(dynamic_kind_a, blem16),
      Builder.CreateICmpEQ(dynamic_kind_b, blem16)
    );
    Value* merged_dynamic_kind =
      Builder.CreateSelect(either_is_dirty, dirty,
      Builder.CreateSelect(either_is_blemother, blemother,
      Builder.CreateSelect(either_is_blem16, blem16,
      clean)));
    return merged_dynamic_kind;
  }
};

inline bool operator==(const PointerStatus& a, const PointerStatus& b) {
  if (a.kind != b.kind) return false;
  if (a.kind == PointerKind::DYNAMIC || b.kind == PointerKind::DYNAMIC) {
    // require dynamic_kinds to be pointer-equal
    if (a.dynamic_kind != b.dynamic_kind) return false;
  }
  return true;
}
inline bool operator!=(const PointerStatus& a, const PointerStatus& b) {
  return !(a == b);
}

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

  /// During non-instrumenting iterations, we don't insert new dynamic
  /// instructions and thus can't create or check `dynamic_kind`s.
  /// During these iterations, `dynamic_kind` must be NULL.
  /// During iterations where `pointer_encoding` is `true`, it must not be NULL.
  void mark_dynamic(const Value* ptr, Value* dynamic_kind) {
    mark_as(ptr, { PointerKind::DYNAMIC, dynamic_kind });
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
      if (ptr->hasName() && ptr->getName().startswith("__DLIM")) {
        // name starts with __DLIM, skip it
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
    : F(F), DL(F.getParent()->getDataLayout()),
      loopinfo(FAM.getResult<LoopAnalysis>(F)), pdtree(FAM.getResult<PostDominatorTreeAnalysis>(F)),
      trustLLVMStructTypes(trustLLVMStructTypes), inttoptr_kind(inttoptr_kind),
      RPOT(ReversePostOrderTraversal<BasicBlock *>(&F.getEntryBlock())) {
    initialize_block_states();
  }
  ~DLIMAnalysis() {
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
    /// Print to a file in ./dlim_dynamic_counts
    TOFILE,
  } DynamicPrintType;

  /// Instruments the code for dynamic counts.
  /// You _must_ run() the analysis first -- instrument() assumes that the
  /// analysis is complete.
  ///
  /// `pointer_encoding`: If `true`, modify the in-memory representation of
  /// pointers so that bits 48 and 49 give information about the pointer status.
  /// This informs dynamic counts.
  void instrument(const bool pointer_encoding, DynamicPrintType print_type) {
    DynamicResults results = initializeDynamicResults();
    IterationResult res = doIteration(NULL, false);
    assert(!res.changed && "Don't run instrument() until analysis has reached fixpoint");
    doIteration(&results, pointer_encoding);
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

  /// This holds the per-block state for the analysis
  class PerBlockState {
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
    DLIMAnalysis::StaticResults static_results;
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
      }
    }

    // Mark pointers to global variables (and other global values, e.g.,
    // functions and IFuncs) as CLEAN in the function's entry block.
    // (If the global variable itself is a pointer, it's still implicitly
    // NOTDEFINEDYET.)
    for (const GlobalValue& gv : F.getParent()->global_values()) {
      assert(gv.getType()->isPointerTy());
      entry_block_pbs->ptrs_beg.mark_clean(&gv);
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
  /// `pointer_encoding`: If `true` (and `dynamic_results` is not NULL), modify
  /// the in-memory representation of pointers so that bits 48 and 49 give
  /// information about the pointer status.  This informs dynamic counts.
  IterationResult doIteration(DynamicResults* dynamic_results, const bool pointer_encoding) {
    StaticResults static_results = { 0 };
    bool changed = false;

    LLVM_DEBUG(dbgs() << "DLIM: starting an iteration through function " << F.getName() << "\n");

    for (BasicBlock* block : RPOT) {
      AnalyzeBlockResult res = analyze_block(*block, dynamic_results, pointer_encoding);
      changed |= res.end_of_block_statuses_changed;
      static_results += res.static_results;
    }

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
    DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   first predecessor has " << ptr_statuses.describe() << " at end\n");
    for (auto it = ++preds, end = pred_end(&block); it != end; ++it) {
      const BasicBlock* otherPred = *it;
      const PerBlockState* otherPred_pbs = block_states[otherPred];
      DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   next predecessor has " << otherPred_pbs->ptrs_end.describe() << " at end\n");
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
  /// `pointer_encoding`: If `true`, modify the in-memory representation of
  /// pointers so that bits 48 and 49 give information about the pointer status.
  /// This informs dynamic counts.
  AnalyzeBlockResult analyze_block(BasicBlock &block, DynamicResults* dynamic_results, const bool pointer_encoding) {
    PerBlockState* pbs = block_states[&block];

    LLVM_DEBUG(
      StringRef blockname;
      if (block.hasName()) {
        blockname = block.getName();
      } else {
        blockname = "<anonymous>";
      }
      dbgs() << "DLIM: analyzing block " << blockname << "\n";
    );
    DEBUG_WITH_TYPE("DLIM-block-previous-state", dbgs() << "DLIM:   this block previously had " << pbs->ptrs_beg.describe() << " at beginning and " << pbs->ptrs_end.describe() << " at end\n");

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
    DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   at beginning of block, we have " << ptr_statuses.describe() << "\n");

    if (!isEntryBlock) {
      // Let's check if that's any different from what we had last time
      if (ptr_statuses.isEqualTo(pbs->ptrs_beg)) {
        // no change. Unless we're adding dynamic instrumentation, there's no
        // need to actually analyze this block. Just return the `StaticResults`
        // we got last time.
        LLVM_DEBUG(dbgs() << "DLIM:   top-of-block statuses haven't changed\n");
        if (!dynamic_results) return AnalyzeBlockResult { false, pbs->static_results };
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
      switch (inst.getOpcode()) {
        case Instruction::Store: {
          StoreInst& store = cast<StoreInst>(inst);
          // first, if we're storing a pointer we have some extra work to do.
          Value* storedVal = store.getValueOperand();
          if (storedVal->getType()->isPointerTy()) {
            // we count the stored pointer for stats purposes
            PointerStatus storedVal_status = ptr_statuses.getStatus(storedVal);
            COUNT_OP_AS_STATUS(store_vals, storedVal_status, &inst, "Storing a pointer");
            // then, if `pointer_encoding`, we modify the store instruction to
            // store the encoded pointer instead.
            // Specifically, when we store the pointer to memory, we use bits
            // 48-49 to indicate its PointerKind, interpreted per the
            // DynamicPointerKind enum.
            // When we later load this pointer from memory, we'll check bits
            // 48-49 to learn the pointer type, then clear them so the pointer
            // is valid for use.
            // (We assume all pointers are userspace pointers, so 48-49 should
            // be 0 for valid pointers.)
            if (pointer_encoding) {
              // create `new_storedVal` which has the appropriate bits set
              IRBuilder<> Builder(&store);
              Value* mask = storedVal_status.to_dynamic_kind_mask(Builder);
              Value* storedVal_as_int = Builder.CreatePtrToInt(storedVal, Builder.getInt64Ty());
              Value* new_storedVal = Builder.CreateOr(storedVal_as_int, mask);
              Value* new_storedVal_as_ptr = Builder.CreateIntToPtr(new_storedVal, storedVal->getType());
              // store the new (encoded) value instead of the old one
              store.setOperand(0, new_storedVal_as_ptr);
              LLVM_DEBUG(dbgs() << "DLIM:   encoded a stored pointer\n");
            }
          }
          // next count the address
          const Value* addr = store.getPointerOperand();
          COUNT_OP_AS_STATUS(store_addrs, ptr_statuses.getStatus(addr), &inst, "Storing to pointer");
          // now, the pointer used as an address becomes clean
          ptr_statuses.mark_clean(addr);
          break;
        }
        case Instruction::Load: {
          LoadInst& load = cast<LoadInst>(inst);
          const Value* ptr = load.getPointerOperand();
          // first count this for static stats
          COUNT_OP_AS_STATUS(load_addrs, ptr_statuses.getStatus(ptr), &inst, "Loading from pointer");
          // now, the pointer becomes clean
          ptr_statuses.mark_clean(ptr);

          if (load.getType()->isPointerTy()) {
            // in this case, we loaded a pointer from memory, so we
            // only know its status dynamically, not statically.
            // See notes above on the Store case.
            if (pointer_encoding) {
              IRBuilder<> BeforeLoad(&load);
              // but we want to insert _after_ the load, not before it
              BasicBlock* bb = BeforeLoad.GetInsertBlock();
              auto ip = BeforeLoad.GetInsertPoint();
              ip++;
              IRBuilder<> AfterLoad(bb, ip);
              Value* val_as_int = AfterLoad.CreatePtrToInt(&load, AfterLoad.getInt64Ty());
              Value* dynamic_kind = AfterLoad.CreateAnd(val_as_int, dynamic_kind_mask);
              Value* new_val = AfterLoad.CreateAnd(val_as_int, ~dynamic_kind_mask);
              Value* new_val_as_ptr = AfterLoad.CreateIntToPtr(new_val, load.getType());
              // replace all uses of `load` with the modified loaded ptr, except
              // of course the use which we just inserted (which generates
              // `val_as_int`)
              load.replaceUsesWithIf(
                new_val_as_ptr,
                [val_as_int](Use &U){ return U.getUser() != val_as_int; }
              );
              ptr_statuses.mark_dynamic(new_val_as_ptr, dynamic_kind);
            } else {
              // when not `pointer_encoding`, we're allowed (and required) to
              // pass NULL here.
              // see notes on `mark_dynamic`
              ptr_statuses.mark_dynamic(&load, NULL);
            }
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
          PointerStatus input_status = ptr_statuses.getStatus(input_ptr);
          InductionPatternResult ipr = isOffsetAnInductionPattern(gep, DL, loopinfo, pdtree);
          if (ipr.is_induction_pattern && ipr.induction_offset.isNonNegative() && ipr.initial_offset.isNonNegative())
          {
            if (ipr.initial_offset.sge(ipr.induction_offset)) {
              ipr.induction_offset = std::move(ipr.initial_offset);
            }
            LLVM_DEBUG(dbgs() << "DLIM:   found an induction GEP with offset effectively constant " << ipr.induction_offset << "\n");
          } else {
            // we don't consider it an induction pattern if it had negative initial and/or induction offsets
            ipr.is_induction_pattern = false;
          }
          GEPResultClassification grc = classifyGEPResult(gep, input_status, DL, trustLLVMStructTypes, ipr.is_induction_pattern ? &ipr.induction_offset : NULL);
          ptr_statuses.mark_as(&gep, grc.classification);
          // if we added a nonzero constant to a pointer, count that for stats purposes
          if (grc.offset_is_constant && grc.constant_offset != zero) {
            COUNT_OP_AS_STATUS(pointer_arith_const, input_status, &gep, "GEP on a pointer");
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
            const PointerStatus true_status = ptr_statuses.getStatus(true_input);
            const PointerStatus false_status = ptr_statuses.getStatus(false_input);
            ptr_statuses.mark_as(&select, PointerStatus::merge(true_status, false_status, &inst));
          }
          break;
        }
        case Instruction::PHI: {
          const PHINode& phi = cast<PHINode>(inst);
          if (phi.getType()->isPointerTy()) {
            SmallVector<std::pair<PointerStatus, BasicBlock*>, 4> incoming_statuses;
            for (const Use& use : phi.incoming_values()) {
              BasicBlock* bb = phi.getIncomingBlock(use);
              auto& ptr_statuses_end_of_bb = block_states[bb]->ptrs_end;
              const Value* value = use.get();
              incoming_statuses.push_back(std::make_pair(
                ptr_statuses_end_of_bb.getStatus(value),
                bb
              ));
            }
            assert(incoming_statuses.size() >= 1);
            // phi: result status is the merger of the statuses of all the inputs
            // in their corresponding blocks
            // or if `pointer_encoding` and all status are dynamic, then we
            // can just add a new PHI to select the correct status
            bool all_incoming_status_are_dynamic = true;
            for (auto& pair : incoming_statuses) {
              PointerStatus& incoming_status = pair.first;
              if (incoming_status.kind != PointerKind::DYNAMIC) {
                all_incoming_status_are_dynamic = false;
                break;
              }
            }
            if (pointer_encoding && all_incoming_status_are_dynamic) {
              IRBuilder<> Builder(&inst);
              PHINode* status_phi = Builder.CreatePHI(Builder.getInt64Ty(), phi.getNumIncomingValues());
              for (auto& pair : incoming_statuses) {
                PointerStatus& incoming_status = pair.first;
                BasicBlock* incoming_bb = pair.second;
                assert(incoming_status.kind == PointerKind::DYNAMIC && "We should only be here if all_incoming_status_are_dynamic");
                assert(incoming_status.dynamic_kind && "when pointer_encoding is true, we shouldn't have dynamic_kind == NULL");
                status_phi->addIncoming(incoming_status.dynamic_kind, incoming_bb);
              }
              assert(status_phi->isComplete());
              ptr_statuses.mark_as(&phi, PointerStatus::dynamic(status_phi));
            } else {
              PointerStatus merged_status = PointerStatus::clean();
              for (auto& pair : incoming_statuses) {
                PointerStatus& incoming_status = pair.first;
                merged_status = PointerStatus::merge(merged_status, incoming_status, &inst);
              }
              ptr_statuses.mark_as(&phi, merged_status);
            }
          }
          break;
        }
        case Instruction::IntToPtr: {
          // if it's already in the map with a kind other than `inttoptr_kind`,
          // then ignore this for stats purposes and don't override the status.
          // (This is so that we ignore `IntToPtr`s which we inserted ourselves
          // as part of `pointer_encoding`.)
          PointerStatus prev_status = ptr_statuses.getStatus(&inst);
          if (prev_status.kind != PointerKind::NOTDEFINEDYET && prev_status.kind != inttoptr_kind) {
            break;
          }
          // count this for stats, and then mark it as `inttoptr_kind`
          static_results.inttoptrs++;
          if (dynamic_results) {
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
    DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   at end of block, we now have " << ptr_statuses.describe() << "\n");
    const bool changed = !ptr_statuses.isEqualTo(pbs->ptrs_end);
    if (changed) {
      DEBUG_WITH_TYPE("DLIM-block-stats", dbgs() << "DLIM:   this was a change\n");
    }
    pbs->ptrs_end = std::move(ptr_statuses);
    pbs->static_results = static_results;
    return AnalyzeBlockResult { changed, static_results };
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
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(clean_mask)),
      dyn_counts.clean,
      null);
    GlobalCounter = Builder.CreateSelect(
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(blemished16_mask)),
      dyn_counts.blemished16,
      GlobalCounter);
    GlobalCounter = Builder.CreateSelect(
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(blemished_other_mask)),
      dyn_counts.blemishedconst,  // conservative, as we don't know which BLEMISHED category to use
      GlobalCounter);
    GlobalCounter = Builder.CreateSelect(
      Builder.CreateICmpEQ(status.dynamic_kind, Builder.getInt64(dirty_mask)),
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
      llvm_unreachable("unexpected print_type\n");
    }

    // Inject this wrapper function into the GlobalDtors for the module
    appendToGlobalDtors(*mod, Wrapper_func, /* Priority = */ 0);
  }
};

PreservedAnalyses StaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, true, PointerKind::CLEAN);
  DLIMAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // StaticDLIMPass only analyzes the IR and doesn't make any changes, so all
  // analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses ParanoidStaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, false, PointerKind::DIRTY);
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

  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, true, PointerKind::CLEAN);
  analysis.run();
  analysis.instrument(true, DLIMAnalysis::DynamicPrintType::TOFILE);

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

  DLIMAnalysis analysis = DLIMAnalysis(F, FAM, true, PointerKind::CLEAN);
  analysis.run();
  analysis.instrument(true, DLIMAnalysis::DynamicPrintType::STDOUT);

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
  bool offsetIsConstant = false;
  // `offset` is only valid if `offsetIsConstant`
  APInt offset = zero;
  if (override_constant_offset == NULL) {
    offsetIsConstant = gep.accumulateConstantOffset(DL, offset);
  } else {
    offsetIsConstant = true;
    offset = *override_constant_offset;
  }

  if (gep.hasAllZeroIndices()) {
    // result of a GEP with all zeroes as indices, is the same as the input pointer.
    assert(offsetIsConstant && offset == zero && "If all indices are constant 0, then the total offset should be constant 0");
    GEPResultClassification grc;
    grc.classification = input_status;
    grc.offset_is_constant = true; // it's a zero constant
    grc.constant_offset = offset;
    return grc;
  }
  if (trustLLVMStructTypes && areAllIndicesTrustworthy(gep)) {
    // nonzero offset, but "trustworthy" offset.
    switch (input_status.kind) {
      case PointerKind::CLEAN: {
        GEPResultClassification grc;
        grc.classification = PointerStatus::clean();
        // we consider this a "zero" constant. For this purpose.
        grc.offset_is_constant = true;
        grc.constant_offset = zero;
        return grc;
      }
      case PointerKind::UNKNOWN: {
        GEPResultClassification grc;
        grc.classification = PointerStatus::unknown();
        // we consider this a "zero" constant. For this purpose.
        grc.offset_is_constant = true;
        grc.constant_offset = zero;
        return grc;
      }
      case PointerKind::DIRTY: {
        GEPResultClassification grc;
        grc.classification = PointerStatus::dirty();
        // we consider this a "zero" constant. For this purpose.
        grc.offset_is_constant = true;
        grc.constant_offset = zero;
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
        GEPResultClassification grc;
        if (input_status.dynamic_kind == NULL) {
          grc.classification = PointerStatus::dynamic(NULL);
        } else {
          // "trustworthy" offset from clean is clean, from dirty is dirty,
          // from BLEMISHED16 is arbitrarily blemished, and from arbitrarily
          // blemished is still arbitrarily blemished.
          IRBuilder<> Builder((GetElementPtrInst*)&gep); // cast to discard const. We should be able to insert stuff before a const instruction.
          Value* dynamic_kind = Builder.CreateSelect(
            Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(blemished16_mask)),
            Builder.getInt64(blemished_other_mask),
            input_status.dynamic_kind
          );
          grc.classification = PointerStatus::dynamic(dynamic_kind);
        }
        // we consider this a "zero" constant. For this purpose.
        grc.offset_is_constant = true;
        grc.constant_offset = zero;
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
  GEPResultClassification grc;
  grc.offset_is_constant = offsetIsConstant;
  if (offsetIsConstant) {
    grc.constant_offset = offset;
    switch (input_status.kind) {
      case PointerKind::CLEAN: {
        // This GEP adds a constant but nonzero amount to a CLEAN
        // pointer. The result is some flavor of BLEMISHED depending
        // on how far the pointer arithmetic goes.
        if (offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          grc.classification = PointerStatus::blemished16();
          return grc;
        } else if (offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (offset.ule(APInt(/* bits = */ 64, /* val = */ 64))) {
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
        if (offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          // Conservatively, the total offset can't exceed 32
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (offset.ule(APInt(/* bits = */ 64, /* val = */ 48))) {
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
        if (offset.ule(APInt(/* bits = */ 64, /* val = */ 32))) {
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
          Value* is_clean = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(clean_mask));
          Value* is_blem16 = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(blemished16_mask));
          Value* is_blemother = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(blemished_other_mask));
          Value* dynamic_kind = Builder.getInt64(dirty_mask);
          dynamic_kind = Builder.CreateSelect(
            is_clean,
            (offset.ule(APInt(/* bits = */ 64, /* val = */ 16))) ?
              Builder.getInt64(blemished16_mask) : // offset <= 16 from a dynamically clean pointer
              Builder.getInt64(blemished_other_mask), // offset >16 from a dynamically clean pointer
            dynamic_kind
          );
          dynamic_kind = Builder.CreateSelect(
            Builder.CreateLogicalOr(is_blem16, is_blemother),
            Builder.getInt64(blemished_other_mask), // any offset from any blemished has to be blemished_other, as we can't prove it stays within blemished16
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
static InductionPatternResult isOffsetAnInductionPattern(
  const GetElementPtrInst &gep,
  const DataLayout &DL,
  const LoopInfo& loopinfo,
  const PostDominatorTree& pdtree
) {
  DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:   Checking the following gep for induction:\n");
  DEBUG_WITH_TYPE("DLIM-loop-induction", gep.dump());
  if (gep.getNumIndices() != 1) return no_induction_pattern; // we only handle simple cases for now
  for (const Use& idx_as_use : gep.indices()) {
    // note that this for loop goes exactly one iteration, due to the check above.
    // `idx` will be the one index of the GEP.
    const Value* idx = idx_as_use.get();
    InductionVarResult ivr = isInductionVar(idx);
    if (ivr.is_induction_var) {
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     GEP single index is an induction var\n");
    } else {
      ValPlusConstantResult vpcr = isValuePlusConstant(idx);
      if (vpcr.valid) {
        // GEP index is `vpcr.value` plus `vpcr.constant`. Let's see if
        // `vpcr.value` is itself an induction variable. This can happen if we
        // are, say, accessing `arr[k+1]` in a loop over `k`
        ivr = isInductionVar(vpcr.value);
        if (ivr.is_induction_var) {
          DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     GEP single index is an induction var plus a constant " << vpcr.constant << "\n");
          ivr.initial_val = ivr.initial_val + vpcr.constant;
          // the first iteration, it's the initial value of the induction variable
          // plus the constant it's always modified by. but the induction increment
          // doesn't care about the constant modification
        }
      }
    }
    if (!ivr.is_induction_var) {
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     not an induction pattern\n");
      return no_induction_pattern;
    }
    // If we get to here, we've found an induction pattern, described by
    // `ivr.initial_val` and `ivr.induction_increment`.
    // However, we still need to ensure that the pointer produced by the GEP
    // actually is guaranteed to be dereferenced during every iteration
    // (resetting it to clean) -- otherwise we can't use the induction reasoning.
    // For now, we conservatively require a stronger property:
    //   - the pointer must be used for one or more load/stores
    //   - at least one of those load/stores must have both of these properties:
    //     - postdominates the GEP
    //     - is inside the loop
    bool success = false;
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
      InductionPatternResult ipr;
      ipr.is_induction_pattern = true;
      ipr.initial_offset = ivr.initial_val * ap_element_size;
      ipr.induction_offset = ivr.induction_increment * ap_element_size;
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     induction pattern with initial " << ipr.initial_offset << " and induction " << ipr.induction_offset << "\n");
      return ipr;
    } else {
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     but failed the dereference-inside-loop check\n");
      return no_induction_pattern;
    }
  }
  llvm_unreachable("should return from inside the for loop");
}

/// Is the given `val` an induction variable?
/// Here, "induction variable" is narrowly defined as:
///     a PHI between a constant (initial value) and a variable (induction)
///     equal to itself plus or minus a constant
static InductionVarResult isInductionVar(const Value* val) {
  if (const PHINode* phi = dyn_cast<PHINode>(val)) {
    bool found_initial_val = false;
    bool found_induction_increment = false;
    APInt initial_val;
    APInt induction_increment;
    for (const Use& use : phi->incoming_values()) {
      const Value* phi_val = use.get();
      if (const Constant* phi_val_const = dyn_cast<Constant>(phi_val)) {
        if (found_initial_val) {
          // two constants in this phi. For now, this isn't a pattern we'll consider for induction.
          return no_induction_var;
        }
        found_initial_val = true;
        initial_val = phi_val_const->getUniqueInteger();
      } else {
        ValPlusConstantResult vpcr = isValuePlusConstant(phi_val);
        if (vpcr.valid) {
          if (found_induction_increment) {
            // two non-constants in this phi. For now, this isn't a pattern we'll consider for induction.
            return no_induction_var;
          }
          // we're looking for the case where we are adding or subbing a
          // constant from the same value
          if (vpcr.value == val) {
            found_induction_increment = true;
            induction_increment = vpcr.constant;
          }
        }
      }
    }
    if (found_initial_val && found_induction_increment) {
      initial_val = initial_val.sextOrSelf(64);
      induction_increment = induction_increment.sextOrSelf(64);
      DEBUG_WITH_TYPE("DLIM-loop-induction", dbgs() << "DLIM:     Found an induction var, initial " << initial_val << " and induction " << induction_increment << "\n");
      InductionVarResult ivr;
      ivr.is_induction_var = true;
      ivr.initial_val = std::move(initial_val);
      ivr.induction_increment = std::move(induction_increment);
      return ivr;
    } else {
      return no_induction_var;
    }
  } else {
    return no_induction_var;
  }
}

/// Is the given `val` defined as some other `Value` plus/minus a constant?
static ValPlusConstantResult isValuePlusConstant(const Value* val) {
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
              return not_a_val_plus_constant;
            }
            found_constant_operand = true;
            constant_val = op_const->getUniqueInteger();
            if (bop->getOpcode() == Instruction::Sub) {
              constant_val = -constant_val;
            }
          } else {
            if (found_nonconstant_operand) {
              // two nonconstant operands
              return not_a_val_plus_constant;
            }
            found_nonconstant_operand = true;
            nonconstant_val = op;
          }
        }
        if (found_constant_operand && found_nonconstant_operand) {
          constant_val = constant_val.sextOrSelf(64);
          ValPlusConstantResult vpcr;
          vpcr.valid = true;
          vpcr.value = std::move(nonconstant_val);
          vpcr.constant = std::move(constant_val);
          return vpcr;
        } else {
          return not_a_val_plus_constant;
        }
      }
      default: {
        return not_a_val_plus_constant;
      }
    }
  } else {
    return not_a_val_plus_constant;
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
