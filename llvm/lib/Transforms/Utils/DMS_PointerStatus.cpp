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

/// Merge two `PointerStatus`es.
/// See comments on PointerKind::merge.
///
/// If we need to insert dynamic instructions to handle the merge, use
/// `Builder`.
/// We will only potentially need to do this if at least one of the statuses
/// is DYNAMIC with a non-null `dynamic_kind`.
PointerStatus PointerStatus::merge(const PointerStatus a, const PointerStatus b, DMSIRBuilder& Builder) {
  if (a.kind == PointerKind::DYNAMIC && b.kind == PointerKind::DYNAMIC) {
    return { PointerKind::DYNAMIC, merge_two_dynamic(a.dynamic_kind, b.dynamic_kind, Builder) };
  } else if (a.kind == PointerKind::DYNAMIC) {
    return merge_static_dynamic(b.kind, a.dynamic_kind, Builder);
  } else if (b.kind == PointerKind::DYNAMIC) {
    return merge_static_dynamic(a.kind, b.dynamic_kind, Builder);
  } else {
    return { PointerKind::merge(a.kind, b.kind), NULL };
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

/// Merge a static `PointerKind` and a `dynamic_kind`.
///
/// This function computes the merger directly, without caching.
static PointerStatus merge_static_dynamic_nocache(
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

/// Used only for the cache in `PointerStatus::merge_static_dynamic`; see notes
/// there
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
/// See comments on PointerStatus::merge.
///
/// This function performs caching. If these two things have been merged before,
/// in the same block (eg on a previous iteration), it returns the cached value;
/// else it computes the merger fresh.
PointerStatus PointerStatus::merge_static_dynamic(
  const PointerKind static_kind,
  Value* dynamic_kind,
  DMSIRBuilder& Builder
) {
  static DenseMap<StaticDynamicCacheKey, PointerStatus> cache;
  StaticDynamicCacheKey Key(static_kind, dynamic_kind, Builder.GetInsertBlock());
  if (cache.count(Key) > 0) return cache[Key];
  PointerStatus merged = merge_static_dynamic_nocache(static_kind, dynamic_kind, Builder);
  cache[Key] = merged;
  return merged;
}

/// Merge two `dynamic_kind`s. Returns a `dynamic_kind`.
///
/// This function computes the merger directly, without caching.
static Value* merge_two_dynamic_nocache(Value* dynamic_kind_a, Value* dynamic_kind_b, DMSIRBuilder& Builder) {
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

/// Used only for the cache in `PointerStatus::merge_two_dynamic`; see notes
/// there
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

// it seems this is required in order for StaticDynamicCacheKey to be a key type
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
/// See comments on PointerStatus::merge.
///
/// This function performs caching. If these two things have been merged before,
/// in the same block (eg on a previous iteration), it returns the cached value;
/// else it computes the merger fresh.
Value* PointerStatus::merge_two_dynamic(Value* dynamic_kind_a, Value* dynamic_kind_b, DMSIRBuilder& Builder) {
  if (dynamic_kind_a == NULL) return dynamic_kind_b;
  if (dynamic_kind_b == NULL) return dynamic_kind_a;
  if (dynamic_kind_a == dynamic_kind_b) return dynamic_kind_a;
  static DenseMap<DynamicDynamicCacheKey, Value*> cache;
  DynamicDynamicCacheKey Key(dynamic_kind_a, dynamic_kind_b, Builder.GetInsertBlock());
  if (cache.count(Key) > 0) return cache[Key];
  Value* merged_dynamic_kind = merge_two_dynamic_nocache(dynamic_kind_a, dynamic_kind_b, Builder);
  cache[Key] = merged_dynamic_kind;
  return merged_dynamic_kind;
}
