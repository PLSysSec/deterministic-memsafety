#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/DMS_mapEquality.h"
#include "llvm/Transforms/Utils/DMS_PointerStatus.h"

namespace llvm {

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
  const PointerStatus inttoptr_status;

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
    const PointerStatus inttoptr_status,
    DenseSet<const Instruction*>& added_insts,
    DenseMap<const Value*, SmallDenseSet<const Value*, 4>>& pointer_aliases
  ) :
    block(block),
    DL(DL),
    trust_llvm_struct_types(trust_llvm_struct_types),
    inttoptr_status(inttoptr_status),
    added_insts(added_insts),
    pointer_aliases(pointer_aliases)
  {}

  PointerStatuses(const PointerStatuses& other) = default;

  PointerStatuses operator=(const PointerStatuses& other) {
    assert(&block == &other.block);
    assert(DL == other.DL);
    assert(trust_llvm_struct_types == other.trust_llvm_struct_types);
    assert(inttoptr_status == other.inttoptr_status);
    assert(&added_insts == &other.added_insts);
    assert(&pointer_aliases == &other.pointer_aliases);
    map = other.map;
    return *this;
  }

  // Use this for any `status` except `NotDefinedYet`
  void mark_as(const Value* ptr, PointerStatus status);

  /// Get the status of `ptr`. If necessary, check its aliases, and those
  /// aliases' aliases, etc
  PointerStatus getStatus(const Value* ptr) const;

private:
  /// Get the status of `ptr`. If necessary, check its aliases, and those
  /// aliases' aliases, etc. However, don't recurse into any aliases listed in
  /// `norecurse`. (We use this to avoid infinite recursion.)
  PointerStatus getStatus_checking_aliases_except(const Value* ptr, SmallDenseSet<const Value*, 4>& norecurse) const;

  /// Get the status of `ptr`. This function doesn't consider aliases of `ptr`;
  /// if `ptr` itself doesn't have a status, returns `PointerStatus::notdefinedyet()`.
  PointerStatus getStatus_noalias(const Value* ptr) const;

  /// Try to get the status of the given `Constant` of integer type.
  ///
  /// PointerStatus only makes sense for the status of a pointer, but this will
  /// still try to do the right thing interpreting the const int as a pointer
  /// value.
  ///
  /// For instance, this can return a sensible status for constant 0 (which is
  /// just NULL), or for integers which are somehow eventually derived from
  /// a pointer via PtrToInt.
  ///
  /// However, this only recognizes a few patterns, so in other cases where it's
  /// not sure, it just won't return a status.
  std::optional<PointerStatus> tryGetStatusOfConstInt(const Constant*) const;

public:
  MapEqualityResult<const Value*> isEqualTo(const PointerStatuses& other) const {
    // since we assert in `mark_as()` that we never explicitly mark anything
    // NOTDEFINEDYET, we can just check that the `map`s contain exactly the same
    // elements mapped to the same things
    return MapEqualityResult<const Value*>::mapsAreEqual(map, other.map);
  }

  std::string describe() const;

  /// Merge the given PointerStatuses. If they disagree on any pointer, use
  /// `PointerStatus::merge_with_phi` to combine the statuses.
  /// Recall that any pointer not appearing in the `map` is considered NOTDEFINEDYET.
  ///
  /// `merge_block`: block where the merge is happening. The merged statuses are for this block.
  static PointerStatuses merge(const SmallVector<const PointerStatuses*, 4>& statuses, BasicBlock& merge_block);
}; // end class PointerStatuses

} // end namespace llvm
