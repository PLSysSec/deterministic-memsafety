#include "llvm/Transforms/Utils/DMS_PointerStatus.h"

#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

/// Merge two `PointerKind`s.
/// For the purposes of this function, the ordering is
/// DIRTY < UNKNOWN < BLEMISHEDCONST < BLEMISHED64 < BLEMISHED32 < BLEMISHED16 < CLEAN,
/// and the merge returns the least element.
/// NOTDEFINEDYET has the property where the merger of x and NOTDEFINEDYET is x
/// (for all x) - for instance, if we are at a join point in the CFG where the
/// pointer is x status on one incoming branch and not defined on the other,
/// the pointer can have x status going forward.
PointerKind PointerKind::merge(const PointerKind a, const PointerKind b) {
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

/// This private function is like PointerKind::merge, but can handle some
/// cases of DYNAMIC. If it knows the merge result will be static, it returns
/// that; if it doesn't know, it returns DYNAMIC.
PointerKind PointerKind::merge_maybe_dynamic(const PointerKind a, const PointerKind b) {
  if (a != DYNAMIC && b != DYNAMIC) {
    return PointerKind::merge(a, b);
  } else if (a == NOTDEFINEDYET) {
    return b;
  } else if (b == NOTDEFINEDYET) {
    return a;
  } else if (a == DIRTY || b == DIRTY) {
    // merging a DIRTY with anything, including a DYNAMIC, will just be DIRTY
    return DIRTY;
  } else if (a == CLEAN) {
    // merging a CLEAN with any x, including a DYNAMIC, will just be that x
    return b;
  } else if (b == CLEAN) {
    // merging a CLEAN with any x, including a DYNAMIC, will just be that x
    return a;
  } else {
    // in other cases, we don't know the status statically, so return DYNAMIC
    return DYNAMIC;
  }
}

ConstantInt* PointerKind::to_constant_dynamic_kind_mask(LLVMContext& ctx) const {
  Type* i64Ty = Type::getInt64Ty(ctx);
  switch (kind) {
    case PointerKind::CLEAN:
      return cast<ConstantInt>(ConstantInt::get(i64Ty, DynamicKindMasks::clean));
    case PointerKind::BLEMISHED16:
      return cast<ConstantInt>(ConstantInt::get(i64Ty, DynamicKindMasks::blemished16));
    case PointerKind::BLEMISHED32:
    case PointerKind::BLEMISHED64:
    case PointerKind::BLEMISHEDCONST:
      return cast<ConstantInt>(ConstantInt::get(i64Ty, DynamicKindMasks::blemished_other));
    case PointerKind::DIRTY:
      return cast<ConstantInt>(ConstantInt::get(i64Ty, DynamicKindMasks::dirty));
    case PointerKind::UNKNOWN:
      // for now we just mark UNKNOWN pointers as dirty when storing them
      return cast<ConstantInt>(ConstantInt::get(i64Ty, DynamicKindMasks::dirty));
    case PointerKind::DYNAMIC:
      llvm_unreachable("to_constant_dynamic_kind_mask can't be called with a DYNAMIC status");
    case PointerKind::NOTDEFINEDYET:
      llvm_unreachable("Shouldn't call to_constant_dynamic_kind_mask on a NOTDEFINEDYET");
    default:
      llvm_unreachable("PointerKind case not handled");
  }
}

Value* PointerStatus::to_dynamic_kind_mask(LLVMContext& ctx) const {
  if (kind == PointerKind::DYNAMIC) {
    return dynamic_kind;
  } else {
    return to_constant_dynamic_kind_mask(ctx);
  }
}

/// Like `to_dynamic_kind_mask()`, but only for `kind`s that aren't `DYNAMIC`.
/// Returns a `ConstantInt` instead of an arbitrary `Value`.
ConstantInt* PointerStatus::to_constant_dynamic_kind_mask(LLVMContext& ctx) const {
  return kind.to_constant_dynamic_kind_mask(ctx);
}

/// Merge two `PointerStatus`es.
/// See comments on PointerKind::merge.
///
/// If we need to insert dynamic instructions to handle the merge, use
/// `Builder`.
/// We will only potentially need to do this if at least one of the statuses
/// is DYNAMIC with a non-null `dynamic_kind`.
PointerStatus PointerStatus::merge_direct(
  const PointerStatus a,
  const PointerStatus b,
  llvm::DMSIRBuilder& Builder
) {
  if (a.kind == PointerKind::DYNAMIC && b.kind == PointerKind::DYNAMIC) {
    return { PointerKind::DYNAMIC, merge_two_dynamic_direct(a.dynamic_kind, b.dynamic_kind, Builder) };
  } else if (a.kind == PointerKind::DYNAMIC) {
    return merge_static_dynamic_direct(b.kind, a.dynamic_kind, Builder);
  } else if (b.kind == PointerKind::DYNAMIC) {
    return merge_static_dynamic_direct(a.kind, b.dynamic_kind, Builder);
  } else {
    return PointerStatus { PointerKind::merge(a.kind, b.kind), NULL };
  }
}

/// Used only for the cache in `PointerStatus::merge_with_phi`; see notes there
struct PhiMergerCacheKey {
  // not a reference or pointer: requires us to copy in the contents of the
  // vector into the vector inside this PhiMergerCacheKey
  SmallVector<StatusWithBlock, 4> statuses;
  BasicBlock* phi_block;

  explicit PhiMergerCacheKey(
    const SmallVector<StatusWithBlock, 4>* _statuses,
    BasicBlock* phi_block
  ) : phi_block(phi_block) {
    if (_statuses) {
      for (StatusWithBlock status : *_statuses) {
        statuses.push_back(StatusWithBlock(status));
      }
    }
  }
  PhiMergerCacheKey() : phi_block(NULL) {}
  PhiMergerCacheKey(const PhiMergerCacheKey& other)
    : statuses(other.statuses), phi_block(other.phi_block) {}

  // https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
  // https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
  friend void swap(PhiMergerCacheKey& A, PhiMergerCacheKey& B) noexcept {
    std::swap(A.statuses, B.statuses);
    std::swap(A.phi_block, B.phi_block);
  }
  PhiMergerCacheKey(PhiMergerCacheKey&& other) noexcept : PhiMergerCacheKey() {
    swap(*this, other);
  }
  PhiMergerCacheKey& operator=(PhiMergerCacheKey rhs) noexcept {
    swap(*this, rhs);
    return *this;
  }

  static inline PhiMergerCacheKey getTombstoneKey() {
    PhiMergerCacheKey Key;
    // getTombstoneKey() doesn't work on a SmallVector, hopefully it's sufficient to have a tombstone in the phi_block field
    Key.phi_block = DenseMapInfo<BasicBlock*>::getTombstoneKey();
    return Key;
  }

  bool operator==(const PhiMergerCacheKey& other) const {
    if (statuses.size() == 0 || phi_block == NULL || other.statuses.size() == 0 || other.phi_block == NULL) {
      if (statuses.size() == 0 && phi_block == NULL && other.statuses.size() == 0 && other.phi_block == NULL) {
        return true;
      } else {
        return false;
      }
    }
    assert(statuses.size() > 0);
    if (statuses.size() != other.statuses.size()) return false;
    if (phi_block != other.phi_block) return false;
    for (size_t i = 0; i < statuses.size(); i++) {
      if (statuses[i].status != other.statuses[i].status) return false;
      if (&statuses[i].block != &other.statuses[i].block) return false;
    }
    return true;
  }
  bool operator!=(const PhiMergerCacheKey& other) const {
    return !(*this == other);
  }
};

// it seems this is required in order for PhiMergerCacheKey to be a key type
// in a DenseMap
namespace llvm {
template<> struct DenseMapInfo<PhiMergerCacheKey> {
  static inline PhiMergerCacheKey getEmptyKey() {
    return PhiMergerCacheKey();
  }
  static inline PhiMergerCacheKey getTombstoneKey() {
    return PhiMergerCacheKey::getTombstoneKey();
  }
  static unsigned getHashValue(const PhiMergerCacheKey &Val) {
    unsigned hash = 0;
    for (const StatusWithBlock& status : Val.statuses) {
      hash ^= DenseMapInfo<BasicBlock*>::getHashValue(status.block);
      hash ^= status.status.kind;
      hash ^= DenseMapInfo<Value*>::getHashValue(status.status.dynamic_kind);
    }
    return hash ^ DenseMapInfo<BasicBlock*>::getHashValue(Val.phi_block);
  }
  static bool isEqual(const PhiMergerCacheKey &LHS, const PhiMergerCacheKey &RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

/// Is A a predecessor of B
static bool block_is_pred_of_block(BasicBlock* A, BasicBlock* B) {
  for (BasicBlock* pred : predecessors(B)) {
    if (pred == A) return true;
  }
  return false;
}

/// Merge a set of `PointerStatus`es.
///
/// If necessary, insert a phi instruction in `phi_block` to perform the
/// merge.
/// We will only potentially need to do this if at least one of the statuses
/// is DYNAMIC with a non-null `dynamic_kind`.
PointerStatus PointerStatus::merge_with_phi(
  const SmallVector<StatusWithBlock, 4>& statuses,
  /// Block where we will insert a phi if necessary (where the merge is occurring)
  BasicBlock* phi_block
) {
  bool have_dynamic_null = false;
  PointerKind maybe_merged = PointerKind::NOTDEFINEDYET;
  for (const StatusWithBlock& swb : statuses) {
    maybe_merged = PointerKind::merge_maybe_dynamic(maybe_merged, swb.status.kind);
    if (swb.status.kind == PointerKind::DYNAMIC) {
      if (swb.status.dynamic_kind == NULL) have_dynamic_null = true;
    }
  }
  // if the result of the merger is a kind known statically, use that.
  // (We could theoretically use a PHI even in this case.
  // This would have the advantage of path-sensitive status instead of
  // a conservative static merger; but the disadvantage of having a dynamic
  // status (from phi) instead of a statically-known (merged) status.
  if (maybe_merged != PointerKind::DYNAMIC) return PointerStatus { maybe_merged, NULL };
  // ok so we don't know the kind statically
  if (have_dynamic_null) {
    // one of the pointers we're merging is dynamic with dynamic_kind NULL.
    // result will be dynamic with dynamic_kind NULL.
    return PointerStatus::dynamic(NULL);
  }
  // another special case to check: if all the statuses are equal, then the
  // merger is trivial
  bool all_equal = true;
  for (size_t i = 0; i < statuses.size() - 1; i++) {
    if (statuses[i].status != statuses[i+1].status) {
      all_equal = false;
      break;
    }
  }
  if (all_equal) return statuses[0].status;
  // at this point it's looking like we have to actually insert a phi.
  // check the cache first
  static SmallDenseMap<PhiMergerCacheKey, PointerStatus, 4> cache;
  PhiMergerCacheKey Key(&statuses, phi_block);
  if (cache.count(Key) > 0) return cache[Key];

  DMSIRBuilder Builder(phi_block, DMSIRBuilder::BEGINNING, NULL);
  PHINode* phi = Builder.CreatePHI(Builder.getInt64Ty(), 2);
  assert(statuses.size() == pred_size(phi_block));
  for (const StatusWithBlock& swb : statuses) {
    assert(block_is_pred_of_block(swb.block, phi_block));
    if (swb.status.kind == PointerKind::NOTDEFINEDYET) {
      // for now, treat this input to the phi as CLEAN.
      // this will be updated on a future iteration if necessary, once the
      // incoming pointer has a defined kind.
      // But if considering this CLEAN leads to the incoming pointer eventually
      // being CLEAN, that's fine, we're happy with that result
      phi->addIncoming(
        Builder.getInt64(DynamicKindMasks::clean),
        swb.block
      );
    } else {
      phi->addIncoming(
        swb.status.to_dynamic_kind_mask(Builder.getContext()),
        swb.block
      );
    }
  }
  assert(phi->isComplete());

  cache[Key] = PointerStatus::dynamic(phi);
  return PointerStatus::dynamic(phi);
}

/// Merge a static `PointerKind` and a `dynamic_kind`, directly (inserting
/// dynamic instructions if necessary).
///
/// This function computes the merger without caching.
static PointerStatus merge_static_dynamic_nocache_direct(
  const PointerKind static_kind,
  Value* dynamic_kind,
  DMSIRBuilder& Builder
) {
  switch (static_kind) {
    case PointerKind::NOTDEFINEDYET:
      // As in PointerKind::merge, merging x with NOTDEFINEDYET is always x
      return PointerStatus::dynamic(dynamic_kind);
    case PointerKind::CLEAN:
      // For all x, merging CLEAN with x results in x
      return PointerStatus::dynamic(dynamic_kind);
    case PointerKind::BLEMISHED16:
      // merging BLEMISHED16 with DYN_CLEAN is DYN_BLEMISHED16.
      // merging BLEMISHED16 with any other x results in x.
      if (dynamic_kind == NULL) return PointerStatus::dynamic(NULL);
      return PointerStatus::dynamic(Builder.CreateSelect(
        Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(DynamicKindMasks::clean)),
        Builder.getInt64(DynamicKindMasks::blemished16),
        dynamic_kind
      ));
    case PointerKind::BLEMISHED32:
    case PointerKind::BLEMISHED64:
    case PointerKind::BLEMISHEDCONST:
      // merging any of these with DYN_CLEAN, DYN_BLEMISHED16, or
      // DYN_BLEMISHEDOTHER results in DYN_BLEMISHEDOTHER.
      // merging any of these with DYN_DIRTY results in DYN_DIRTY.
      if (dynamic_kind == NULL) return PointerStatus::dynamic(NULL);
      return PointerStatus::dynamic(Builder.CreateSelect(
        Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(DynamicKindMasks::dirty)),
        Builder.getInt64(DynamicKindMasks::dirty),
        Builder.getInt64(DynamicKindMasks::blemished_other)
      ));
    case PointerKind::DIRTY:
    case PointerKind::UNKNOWN:
      // merging anything with DIRTY or UNKNOWN results in DIRTY
      return PointerStatus::dirty();
    case PointerKind::DYNAMIC:
      llvm_unreachable("merge_static_dynamic: expected a static PointerKind");
    default:
      llvm_unreachable("Missing PointerKind case");
  }
}

/// Used only for the cache in `PointerStatus::merge_static_dynamic_direct`; see
/// notes there
struct StaticDynamicCacheKey {
  PointerKind static_kind;
  Value* dynamic_kind;
  BasicBlock* block;

  StaticDynamicCacheKey(
    PointerKind static_kind,
    Value* dynamic_kind,
    BasicBlock* block
  ) : static_kind(static_kind), dynamic_kind(dynamic_kind), block(block)
  {}

  bool operator==(const StaticDynamicCacheKey& other) const {
    return
      static_kind == other.static_kind &&
      dynamic_kind == other.dynamic_kind &&
      block == other.block;
  }
  bool operator!=(const StaticDynamicCacheKey& other) const {
    return !(*this == other);
  }
};

// it seems this is required in order for StaticDynamicCacheKey to be a key type
// in a DenseMap
namespace llvm {
template<> struct DenseMapInfo<StaticDynamicCacheKey> {
  static inline StaticDynamicCacheKey getEmptyKey() {
    return StaticDynamicCacheKey(PointerKind::NOTDEFINEDYET, NULL, NULL);
  }
  static inline StaticDynamicCacheKey getTombstoneKey() {
    return StaticDynamicCacheKey(
      PointerKind::NOTDEFINEDYET,
      DenseMapInfo<Value*>::getTombstoneKey(),
      DenseMapInfo<BasicBlock*>::getTombstoneKey()
    );
  }
  static unsigned getHashValue(const StaticDynamicCacheKey &Val) {
    return
      DenseMapInfo<Value*>::getHashValue(Val.dynamic_kind) ^
      DenseMapInfo<BasicBlock*>::getHashValue(Val.block) ^
      Val.static_kind;
  }
  static bool isEqual(const StaticDynamicCacheKey &LHS, const StaticDynamicCacheKey &RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

/// Merge a static `PointerKind` and a `dynamic_kind`.
/// See comments on PointerStatus::merge_direct.
///
/// This function performs caching. If these two things have been merged before,
/// in the same block (eg on a previous iteration), it returns the cached value;
/// else it computes the merger fresh.
PointerStatus PointerStatus::merge_static_dynamic_direct(
  const PointerKind static_kind,
  Value* dynamic_kind,
  DMSIRBuilder& Builder
) {
  static DenseMap<StaticDynamicCacheKey, PointerStatus> cache;
  StaticDynamicCacheKey Key(static_kind, dynamic_kind, Builder.GetInsertBlock());
  if (cache.count(Key) > 0) return cache[Key];
  PointerStatus merged = merge_static_dynamic_nocache_direct(static_kind, dynamic_kind, Builder);
  cache[Key] = merged;
  return merged;
}

/// Merge two `dynamic_kind`s, inserting dynamic instructions if necessary.
/// Returns a `dynamic_kind`.
///
/// This function computes the merger without caching.
static Value* merge_two_dynamic_nocache_direct(
  Value* dynamic_kind_a,
  Value* dynamic_kind_b,
  DMSIRBuilder& Builder
) {
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
  Value* dirty = Builder.getInt64(DynamicKindMasks::dirty);
  Value* blemother = Builder.getInt64(DynamicKindMasks::blemished_other);
  Value* blem16 = Builder.getInt64(DynamicKindMasks::blemished16);
  Value* clean = Builder.getInt64(DynamicKindMasks::clean);
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

/// Used only for the cache in `PointerStatus::merge_two_dynamic_direct`; see
/// notes there
struct DynamicDynamicCacheKey {
  Value* dynamic_kind_a;
  Value* dynamic_kind_b;
  BasicBlock* block;

  DynamicDynamicCacheKey(
    Value* dynamic_kind_a,
    Value* dynamic_kind_b,
    BasicBlock* block
  ) : dynamic_kind_a(dynamic_kind_a), dynamic_kind_b(dynamic_kind_b), block(block)
  {}

  bool operator==(const DynamicDynamicCacheKey& other) const {
    return
      dynamic_kind_a == other.dynamic_kind_a &&
      dynamic_kind_b == other.dynamic_kind_b &&
      block == other.block;
  }
  bool operator!=(const DynamicDynamicCacheKey& other) const {
    return !(*this == other);
  }
};

// it seems this is required in order for DynamicDynamicCacheKey to be a key type
// in a DenseMap
namespace llvm {
template<> struct DenseMapInfo<DynamicDynamicCacheKey> {
  static inline DynamicDynamicCacheKey getEmptyKey() {
    return DynamicDynamicCacheKey(NULL, NULL, NULL);
  }
  static inline DynamicDynamicCacheKey getTombstoneKey() {
    return DynamicDynamicCacheKey(
      DenseMapInfo<Value*>::getTombstoneKey(),
      DenseMapInfo<Value*>::getTombstoneKey(),
      DenseMapInfo<BasicBlock*>::getTombstoneKey()
    );
  }
  static unsigned getHashValue(const DynamicDynamicCacheKey &Val) {
    return
      DenseMapInfo<Value*>::getHashValue(Val.dynamic_kind_a) ^
      DenseMapInfo<Value*>::getHashValue(Val.dynamic_kind_b) ^
      DenseMapInfo<BasicBlock*>::getHashValue(Val.block);
  }
  static bool isEqual(const DynamicDynamicCacheKey &LHS, const DynamicDynamicCacheKey &RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

/// Merge two `dynamic_kind`s.
/// See comments on PointerStatus::merge_direct.
///
/// This function performs caching. If these two things have been merged before,
/// in the same block (eg on a previous iteration), it returns the cached value;
/// else it computes the merger fresh.
Value* PointerStatus::merge_two_dynamic_direct(Value* dynamic_kind_a, Value* dynamic_kind_b, DMSIRBuilder& Builder) {
  if (dynamic_kind_a == NULL) return dynamic_kind_b;
  if (dynamic_kind_b == NULL) return dynamic_kind_a;
  if (dynamic_kind_a == dynamic_kind_b) return dynamic_kind_a;
  static DenseMap<DynamicDynamicCacheKey, Value*> cache;
  DynamicDynamicCacheKey Key(dynamic_kind_a, dynamic_kind_b, Builder.GetInsertBlock());
  if (cache.count(Key) > 0) return cache[Key];
  Value* merged_dynamic_kind = merge_two_dynamic_nocache_direct(dynamic_kind_a, dynamic_kind_b, Builder);
  cache[Key] = merged_dynamic_kind;
  return merged_dynamic_kind;
}
