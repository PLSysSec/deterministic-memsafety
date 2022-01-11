#ifndef DMS_POINTERSTATUS_H
#define DMS_POINTERSTATUS_H

#include "llvm/ADT/APInt.h"
#include "llvm/Transforms/Utils/DMS_IRBuilder.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <optional>
#include <variant>

struct StatusWithBlock;

/// PointerStatus describes the status of a single pointer at a given program
/// point. Or, it can be used to describe the status of a vector of pointers, if
/// the status applies (conservatively) to all pointers in the vector
/// individually.
class PointerStatus final {
public:
  // A `PointerStatus` can take one of these forms:

  /// Unknown status.
  /// As of this writing, two important ways Unknowns arise are: returning a
  /// pointer from a call; or receiving a pointer as a function parameter
  struct Unknown {
    // no additional information needed
    Unknown() = default;
    bool operator==(const Unknown& other) const { return true; }
    bool operator!=(const Unknown& other) const { return !(*this == other); }
  };

  /// `Clean` means "not modified since last allocated or dereferenced", or for
  /// some other reason we know it is in-bounds
  struct Clean {
    // no additional information needed
    Clean() = default;
    bool operator==(const Clean& other) const { return true; }
    bool operator!=(const Clean& other) const { return !(*this == other); }
  };

  /// `Blemished` means "incremented by some constant amount from a clean
  /// pointer".
  struct Blemished {
    /// If this value is present, it is the (total) amount by which this pointer
    /// has been incremented, in bytes, since we last knew it was `Clean`.
    ///
    /// If we're unsure, this is the most conservative (largest) amount by which
    /// it could have been incremented, in bytes, since we last knew it was
    /// `Clean`.
    ///
    /// If the pointer has been (in total) decremented since we last knew it was
    /// `Clean`, or if we're unsure but some of the possible (total)
    /// modifications are negative, then this will be std::nullopt.
    /// `max_modification` should never be negative.
    ///
    /// If the pointer has been (or may have been) modified by a
    /// non-compile-time-constant, it gets `Dirty` instead of `Blemished`.
    std::optional<llvm::APInt> max_modification;

    /// Returns `true` if this `Blemished` has a max_modification value and it's
    /// <=16.
    bool under16() const {
      return max_modification.has_value() && max_modification->ule(16);
    }

    std::string pretty() const;

    Blemished() : max_modification(std::nullopt) {}
    Blemished(llvm::APInt max_modification) : max_modification(max_modification) {}
    Blemished(std::optional<llvm::APInt> max_modification) : max_modification(max_modification) {}
    bool operator==(const Blemished& other) const { return max_modification == other.max_modification; }
    bool operator!=(const Blemished& other) const { return !(*this == other); }
  };

  /// `Dirty` means "may have been incremented/decremented by a
  /// non-compile-time-constant amount since we last knew it was `Clean`"
  struct Dirty {
    // no additional information needed
    Dirty() = default;
    bool operator==(const Dirty& other) const { return true; }
    bool operator!=(const Dirty& other) const { return !(*this == other); }
  };

  /// `Dynamic` means that we don't know the status statically, but the status
  /// is indicated in an LLVM variable. Currently, the only operation producing
  /// `Dynamic` is loading a pointer from memory.
  struct Dynamic {
    /// LLVM Value holding information about the dynamic status.
    ///
    /// This will be a Value of type i64, where all bits are zeroes except
    /// possibly bits 48 and 49, whose value together indicate the dynamic kind
    /// according to DynamicKind (below).
    ///
    /// This may be NULL before `pointer_encoding_is_complete` -- in
    /// particular if we're not doing pointer encoding at all.
    /// (If we aren't inserting dynamic instructions for pointer encoding, we
    /// don't have the dynamic Values representing DynamicKinds.)
    /// If/when `pointer_encoding_is_complete`, this must not be NULL.
    llvm::Value* dynamic_kind;

    /// Interpretations for each possible value of bits 48 and 49 in
    /// `dynamic_kind`
    enum DynamicKind {
      /// indicates `Clean`
      DYN_CLEAN = 3,
      /// indicates `Blemished` with <=16
      DYN_BLEMISHED16 = 1,
      /// indicates any other `Blemished`
      DYN_BLEMISHEDOTHER = 2,
      /// indicates anything else, which we conservatively interpret as `Dirty`
      DYN_DIRTY = 0,
    };

    struct Masks final {
      static const uint64_t dirty = ((uint64_t)DYN_DIRTY) << 48;
      static const uint64_t blemished16 = ((uint64_t)DYN_BLEMISHED16) << 48;
      static const uint64_t blemished_other = ((uint64_t)DYN_BLEMISHEDOTHER) << 48;
      static const uint64_t clean = ((uint64_t)DYN_CLEAN) << 48;
      static const uint64_t dynamic_kind_mask = ((uint64_t)0b11) << 48;
    };

    Dynamic(llvm::Value* dynamic_kind) : dynamic_kind(dynamic_kind) {}
    bool operator==(const Dynamic& other) const {
      // require dynamic kinds to be pointer-equal
      return dynamic_kind == other.dynamic_kind;
    }
    bool operator!=(const Dynamic& other) const { return !(*this == other); }
  };

  /// `NotDefinedYet` means that the pointer has not been defined yet at this
  /// program point (at least, to our current knowledge). All pointers are
  /// (effectively) initialized to `NotDefinedYet` at the beginning of the
  /// fixpoint analysis, and as we iterate we gradually refine this.
  struct NotDefinedYet {
    // no additional information needed
    NotDefinedYet() = default;
    bool operator==(const NotDefinedYet& other) const { return true; }
    bool operator!=(const NotDefinedYet& other) const { return !(*this == other); }
  };

  /// Actual data held in the `PointerStatus`: one of the above forms
  std::variant<Unknown, Clean, Blemished, Dirty, Dynamic, NotDefinedYet> data;

  bool valid() const {
    return !data.valueless_by_exception();
  }

  bool is_unknown() const {
    assert(valid());
    return std::holds_alternative<Unknown>(data);
  }

  bool is_clean() const {
    assert(valid());
    return std::holds_alternative<Clean>(data);
  }

  bool is_blemished() const {
    assert(valid());
    return std::holds_alternative<Blemished>(data);
  }

  bool is_dirty() const {
    assert(valid());
    return std::holds_alternative<Dirty>(data);
  }

  bool is_dynamic() const {
    assert(valid());
    return std::holds_alternative<Dynamic>(data);
  }

  bool is_notdefinedyet() const {
    assert(valid());
    return std::holds_alternative<NotDefinedYet>(data);
  }

  PointerStatus(Unknown data) : data(data) {}
  static PointerStatus unknown() {
    return Unknown();
  }

  PointerStatus(Clean data) : data(data) {}
  static PointerStatus clean() {
    return Clean();
  }

  PointerStatus(Blemished data) : data(data) {}
  static PointerStatus blemished() {
    return Blemished();
  }
  static PointerStatus blemished(llvm::APInt max_modification) {
    return Blemished(max_modification);
  }
  static PointerStatus blemished(std::optional<llvm::APInt> max_modification) {
    return Blemished(max_modification);
  }

  PointerStatus(Dirty data) : data(data) {}
  static PointerStatus dirty() {
    return Dirty();
  }

  PointerStatus(Dynamic data) : data(data) {}
  static PointerStatus dynamic(llvm::Value* dynamic_kind) {
    return Dynamic(dynamic_kind);
  }

  PointerStatus(NotDefinedYet data) : data(data) {}
  static PointerStatus notdefinedyet() {
    return NotDefinedYet();
  }

  /// The default constructor produces a NotDefinedYet
  explicit PointerStatus() : data(NotDefinedYet()) {}

  bool operator==(const PointerStatus& other) const {
    return (data == other.data);
  }
  bool operator!=(const PointerStatus& other) const {
    return !(*this == other);
  }

  std::string pretty() const;

  /// This is for DenseMap, see notes where it's used
  static unsigned getHashValue(const PointerStatus& status);

  llvm::Value* to_dynamic_kind_mask(llvm::LLVMContext& ctx) const;

  /// Like `to_dynamic_kind_mask()`, but only when `data` isn't `Dynamic`.
  /// Returns a `ConstantInt` instead of an arbitrary `Value`.
  llvm::ConstantInt* to_constant_dynamic_kind_mask(llvm::LLVMContext& ctx) const;

  /// Merge two `PointerStatus`es.
  /// For the purposes of this function, the ordering is
  /// `Dirty` < `Unknown` < `Blemished`(nullopt) < `Blemished`(larger value)
  ///   < `Blemished`(smaller value) < `Clean`,
  /// and the merge returns the least element.
  /// `NotDefinedYet` has the property where the merger of x and `NotDefinedYet`
  /// is x (for all x) - for instance, if we are at a join point in the CFG
  /// where the pointer is x status on one incoming branch and not defined on
  /// the other, the pointer can have x status going forward.
  ///
  /// If we need to insert dynamic instructions to handle the merge, use
  /// `Builder`.
  /// If neither of the statuses are `Dynamic` with non-null `dynamic_kind`,
  /// then no dynamic instructions will be inserted and `Builder` may be NULL.
  /// Otherwise, dynamic instructions may be inserted.
  static PointerStatus merge_direct(
    const PointerStatus A,
    const PointerStatus B,
    llvm::DMSIRBuilder* Builder
  );

  /// Merge a set of `PointerStatus`es for the given `ptr` in `phi_block`.
  ///
  /// (See general comments on merging on `merge_direct`.)
  ///
  /// If necessary, insert a phi instruction in `phi_block` to perform the
  /// merge.
  /// We will only potentially need to do this if at least one of the statuses
  /// is `Dynamic` with a non-null `dynamic_kind`.
  static PointerStatus merge_with_phi(
    const llvm::SmallVector<StatusWithBlock, 4>& statuses,
    const llvm::Value* ptr,
    llvm::BasicBlock* phi_block
  );

private:
  /// Merge a static `PointerStatus` and a `dynamic_kind`.
  /// See comments on PointerStatus::merge_direct.
  static PointerStatus merge_static_dynamic_direct(
    const PointerStatus static_status,
    llvm::Value* dynamic_kind,
    /// Builder to insert instructions with (if necessary)
    llvm::DMSIRBuilder& Builder
  );

  /// Merge a static `PointerStatus` and a `dynamic_kind`.
  /// See comments on PointerStatus::merge_direct.
  ///
  /// This function computes the merger without caching.
  static PointerStatus merge_static_dynamic_nocache_direct(
    const PointerStatus static_status,
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
