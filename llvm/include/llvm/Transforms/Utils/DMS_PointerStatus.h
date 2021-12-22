#ifndef DMS_POINTERSTATUS_H
#define DMS_POINTERSTATUS_H

#include "llvm/Transforms/Utils/DMS_IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

class PointerKind final {
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

  const char* pretty() const {
    switch (kind) {
      case UNKNOWN: return "UNKNOWN";
      case CLEAN: return "CLEAN";
      case BLEMISHED16: return "BLEMISHED16";
      case BLEMISHED32: return "BLEMISHED32";
      case BLEMISHED64: return "BLEMISHED64";
      case BLEMISHEDCONST: return "BLEMISHEDCONST";
      case DIRTY: return "DIRTY";
      case DYNAMIC: return "DYNAMIC";
      case NOTDEFINEDYET: return "NOTDEFINEDYET";
      default: llvm_unreachable("Missing PointerKind case");
    }
  }

  llvm::ConstantInt* to_constant_dynamic_kind_mask(llvm::LLVMContext& ctx) const;

  /// Merge two `PointerKind`s.
  /// For the purposes of this function, the ordering is
  /// DIRTY < UNKNOWN < BLEMISHEDCONST < BLEMISHED64 < BLEMISHED32 < BLEMISHED16 < CLEAN,
  /// and the merge returns the least element.
  /// NOTDEFINEDYET has the property where the merger of x and NOTDEFINEDYET is x
  /// (for all x) - for instance, if we are at a join point in the CFG where the
  /// pointer is x status on one incoming branch and not defined on the other,
  /// the pointer can have x status going forward.
  static PointerKind merge(const PointerKind a, const PointerKind b);

private:
  Kind kind;

  /// This private function is like PointerKind::merge, but can handle some
  /// cases of DYNAMIC. If it knows the merge result will be static, it returns
  /// that; if it doesn't know, it returns DYNAMIC.
  static PointerKind merge_maybe_dynamic(const PointerKind a, const PointerKind b);
  friend class PointerStatus;
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

class DynamicKindMasks final {
public:
  static const uint64_t dirty = ((uint64_t)DYN_DIRTY) << 48;
  static const uint64_t blemished16 = ((uint64_t)DYN_BLEMISHED16) << 48;
  static const uint64_t blemished_other = ((uint64_t)DYN_BLEMISHEDOTHER) << 48;
  static const uint64_t clean = ((uint64_t)DYN_CLEAN) << 48;
  static const uint64_t dynamic_kind_mask = ((uint64_t)0b11) << 48;
};

struct StatusWithBlock;

/// In principle, and perhaps literally in the future with std::variant,
/// PointerKind::DYNAMIC carries an LLVM Value*.
/// (If we do switch to std::variant, maybe BLEMISHED would carry an int
/// indicating exactly how blemished.)
/// For now, we have this, representing the ADT.
class PointerStatus final {
public:
  /// the PointerKind
  PointerKind kind;
  /// Only for PointerKind::DYNAMIC, this holds the dynamic kind. This will be a
  /// Value of type i64, where all bits are zeroes except possibly bits 48 and
  /// 49, whose value together indicate the dynamic kind according to
  /// DynamicPointerKind (above).
  ///
  /// This field is undefined if `kind` is not `DYNAMIC`.
  ///
  /// This field may be NULL before `pointer_encoding_is_complete` -- in
  /// particular if we're not doing pointer encoding at all.
  /// (If we aren't inserting dynamic instructions for pointer encoding, we
  /// don't have the dynamic Values representing dynamic kinds.)
  /// If/when `pointer_encoding_is_complete`, this field must not be NULL.
  llvm::Value* dynamic_kind;

  static PointerStatus unknown() { return { PointerKind::UNKNOWN, NULL }; }
  static PointerStatus clean() { return { PointerKind::CLEAN, NULL }; }
  static PointerStatus blemished16() { return { PointerKind::BLEMISHED16, NULL }; }
  static PointerStatus blemished32() { return { PointerKind::BLEMISHED32, NULL }; }
  static PointerStatus blemished64() { return { PointerKind::BLEMISHED64, NULL }; }
  static PointerStatus blemishedconst() { return { PointerKind::BLEMISHEDCONST, NULL }; }
  static PointerStatus dirty() { return { PointerKind::DIRTY, NULL }; }
  static PointerStatus notdefinedyet() { return { PointerKind::NOTDEFINEDYET, NULL }; }
  static PointerStatus dynamic(llvm::Value* dynamic_kind) { return { PointerKind::DYNAMIC, dynamic_kind }; }

  /// Create a `PointerStatus` from a `PointerKind`. If the `PointerKind` is `DYNAMIC`,
  /// the `PointerStatus` will have dynamic_kind `NULL`; see notes on `dynamic_kind`.
  static PointerStatus from_kind(PointerKind kind) { return { kind, NULL }; }

  bool operator==(const PointerStatus& other) const {
    if (kind != other.kind) return false;
    if (kind == PointerKind::DYNAMIC) {
      // require dynamic_kinds to be pointer-equal
      if (dynamic_kind != other.dynamic_kind) return false;
    }
    return true;
  }
  bool operator!=(const PointerStatus& other) const {
    return !(*this == other);
  }

  std::string pretty() const { return kind.pretty(); }

  llvm::Value* to_dynamic_kind_mask(llvm::LLVMContext& ctx) const;

  /// Like `to_dynamic_kind_mask()`, but only for `kind`s that aren't `DYNAMIC`.
  /// Returns a `ConstantInt` instead of an arbitrary `Value`.
  llvm::ConstantInt* to_constant_dynamic_kind_mask(llvm::LLVMContext& ctx) const;

  /// Merge two `PointerStatus`es.
  /// See comments on PointerKind::merge.
  ///
  /// If we need to insert dynamic instructions to handle the merge, use
  /// `Builder`.
  /// We will only potentially need to do this if at least one of the statuses
  /// is DYNAMIC with a non-null `dynamic_kind`.
  static PointerStatus merge_direct(
    const PointerStatus a,
    const PointerStatus b,
    llvm::DMSIRBuilder& Builder
  );

  /// Merge a set of `PointerStatus`es for the given `ptr` in `phi_block`.
  ///
  /// If necessary, insert a phi instruction in `phi_block` to perform the
  /// merge.
  /// We will only potentially need to do this if at least one of the statuses
  /// is DYNAMIC with a non-null `dynamic_kind`.
  static PointerStatus merge_with_phi(
    const llvm::SmallVector<StatusWithBlock, 4>& statuses,
    const llvm::Value* ptr,
    llvm::BasicBlock* phi_block
  );

private:
  /// Merge a static `PointerKind` and a `dynamic_kind`.
  /// See comments on PointerStatus::merge_direct.
  static PointerStatus merge_static_dynamic_direct(
    const PointerKind static_kind,
    llvm::Value* dynamic_kind,
    /// Builder to insert instructions with (if necessary)
    llvm::DMSIRBuilder& Builder
  );

  /// Merge two `dynamic_kind`s.
  /// See comments on PointerStatus::merge_direct.
  static llvm::Value* merge_two_dynamic_direct(
    llvm::Value* dynamic_kind_a,
    llvm::Value* dynamic_kind_b,
    /// Builder to insert instructions with (if necessary)
    llvm::DMSIRBuilder& Builder
  );
};

/// Holds a `PointerStatus` and the `BasicBlock` which it came from
struct StatusWithBlock {
  PointerStatus status;
  llvm::BasicBlock* block;

  StatusWithBlock(const PointerStatus status, llvm::BasicBlock* block)
    : status(status), block(block) {}
};

#endif
