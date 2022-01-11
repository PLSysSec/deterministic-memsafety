#include "llvm/Transforms/Utils/DMS_PointerStatus.h"

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

std::string PointerStatus::pretty() const {
  if (!valid()) return "<invalid>";
  if (const Blemished* blem = std::get_if<Blemished>(&data)) {
    return blem->pretty();
  } else if (is_unknown()) {
    return "Unknown";
  } else if (is_clean()) {
    return "Clean";
  } else if (is_dirty()) {
    return "Dirty";
  } else if (is_dynamic()) {
    return "Dynamic";
  } else if (is_notdefinedyet()) {
    return "NotDefinedYet";
  } else {
    llvm_unreachable("PointerStatus case not handled");
  }
}

std::string PointerStatus::Blemished::pretty() const {
  if (max_modification.has_value()) {
    std::string out;
    raw_string_ostream ostream(out);
    ostream << "Blemished (";
    max_modification->print(ostream, /* isSigned = */ false);
    ostream << ")";
    return ostream.str();
  } else {
    return "Blemished (otherconst)";
  }
}

/// Like `to_dynamic_kind_mask()`, but only when `data` isn't `Dynamic`.
/// Returns a `ConstantInt` instead of an arbitrary `Value`.
ConstantInt* PointerStatus::to_constant_dynamic_kind_mask(LLVMContext& ctx) const {
  Type* i64Ty = Type::getInt64Ty(ctx);
  if (const Clean* clean = std::get_if<Clean>(&data)) {
    (void)clean; // silence warning about unused variable
    return cast<ConstantInt>(ConstantInt::get(i64Ty, Dynamic::Masks::clean));
  } else if (const Blemished* blem = std::get_if<Blemished>(&data)) {
    if (blem->under16()) {
      return cast<ConstantInt>(ConstantInt::get(i64Ty, Dynamic::Masks::blemished16));
    } else {
      return cast<ConstantInt>(ConstantInt::get(i64Ty, Dynamic::Masks::blemished_other));
    }
  } else if (const Dirty* dirty = std::get_if<Dirty>(&data)) {
    (void)dirty; // silence warning about unused variable
    return cast<ConstantInt>(ConstantInt::get(i64Ty, Dynamic::Masks::dirty));
  } else if (const Unknown* unk = std::get_if<Unknown>(&data)) {
    // for now we just mark UNKNOWN pointers as dirty when storing them
    (void)unk; // silence warning about unused variable
    return cast<ConstantInt>(ConstantInt::get(i64Ty, Dynamic::Masks::dirty));
  } else if (const Dynamic* dyn = std::get_if<Dynamic>(&data)) {
    (void)dyn; // silence warning about unused variable
    llvm_unreachable("to_constant_dynamic_kind_mask can't be called with a `Dynamic` status");
  } else if (const NotDefinedYet* ndy = std::get_if<NotDefinedYet>(&data)) {
    (void)ndy; // silence warning about unused variable
    llvm_unreachable("Shouldn't call to_constant_dynamic_kind_mask on a `NotDefinedYet`");
  } else {
    llvm_unreachable("PointerStatus case not handled");
  }
}

Value* PointerStatus::to_dynamic_kind_mask(LLVMContext& ctx) const {
  if (const Dynamic* dyn = std::get_if<Dynamic>(&data)) {
    return dyn->dynamic_kind;
  } else {
    return to_constant_dynamic_kind_mask(ctx);
  }
}

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
PointerStatus PointerStatus::merge_direct(
  const PointerStatus A,
  const PointerStatus B,
  llvm::DMSIRBuilder* Builder
) {
  if (A.is_dynamic() && B.is_dynamic()) {
    assert(Builder);
    auto A_dyn = std::get<Dynamic>(A.data);
    auto B_dyn = std::get<Dynamic>(B.data);
    return Dynamic(merge_two_dynamic_direct(A_dyn.dynamic_kind, B_dyn.dynamic_kind, *Builder));
  } else if (A.is_dynamic()) {
    assert(Builder);
    auto A_dyn = std::get<Dynamic>(A.data);
    return merge_static_dynamic_direct(B, A_dyn.dynamic_kind, *Builder);
  } else if (B.is_dynamic()) {
    assert(Builder);
    auto B_dyn = std::get<Dynamic>(B.data);
    return merge_static_dynamic_direct(A, B_dyn.dynamic_kind, *Builder);
  } else {
    // no dynamic statuses involved in this merger.
    if (A.is_notdefinedyet()) {
      return B;
    } else if (B.is_notdefinedyet()) {
      return A;
    } else if (A.is_dirty() || B.is_dirty()) {
      return dirty();
    } else if (A.is_unknown() || B.is_unknown()) {
      return unknown();
    } else if (A.is_blemished() && B.is_blemished()) {
      auto A_blem = std::get<Blemished>(A.data);
      auto B_blem = std::get<Blemished>(B.data);
      if (!A_blem.max_modification.has_value() || !B_blem.max_modification.has_value()) {
        return blemished();
      } else {
        if (A_blem.max_modification->uge(*B_blem.max_modification)) {
          return A;
        } else {
          return B;
        }
      }
    } else if (A.is_blemished()) {
      return A;
    } else if (B.is_blemished()) {
      return B;
    } else if (A.is_clean() && B.is_clean()) {
      return clean();
    } else {
      llvm_unreachable("Missing case in merge function");
    }
  }
}

/// This private function is like PointerStatus::merge_direct, but never inserts
/// dynamic instructions. If the merge result is known statically, it returns
/// that; if the merge must be done dynamically, it returns `Dynamic(NULL)`
/// instead of doing the merge.
static PointerStatus merge_maybe_dynamic(
  const PointerStatus A,
  const PointerStatus B
) {
  if (!A.is_dynamic() && !B.is_dynamic()) {
    return PointerStatus::merge_direct(A, B, NULL);
  } else if (A.is_notdefinedyet()) {
    return B;
  } else if (B.is_notdefinedyet()) {
    return A;
  } else if (A.is_dirty() || B.is_dirty()) {
    // merging a `Dirty` with anything, including a `Dynamic`, will just be `Dirty`
    return PointerStatus::dirty();
  } else if (A.is_clean()) {
    // merging a `Clean` with any x, including a `Dynamic`, will just be that x
    return B;
  } else if (B.is_clean()) {
    // merging a `Clean` with any x, including a `Dynamic`, will just be that x
    return A;
  } else {
    // in other cases, we don't know the merged status statically, so return `Dynamic(NULL)`
    return PointerStatus::dynamic(NULL);
  }
}

/// This is for DenseMap, see notes where it's used
unsigned PointerStatus::getHashValue(const PointerStatus& status) {
  if (status.is_blemished()) {
    const Blemished& blem = std::get<Blemished>(status.data);
    if (blem.max_modification.has_value()) {
      return status.data.index() ^ DenseMapInfo<APInt>::getHashValue(*blem.max_modification);
    } else {
      return status.data.index() ^ (-1);
    }
  } else if (status.is_dynamic()) {
    const Dynamic& dyn = std::get<Dynamic>(status.data);
    return status.data.index() ^ DenseMapInfo<Value*>::getHashValue(dyn.dynamic_kind);
  } else {
    return status.data.index();
  }
}

/// Used only for the cache in `PointerStatus::merge_with_phi`; see notes there
struct PhiMergerCacheKey {
  const Value* ptr;
  BasicBlock* phi_block;

  explicit PhiMergerCacheKey(const Value* ptr, BasicBlock* phi_block)
    : ptr(ptr), phi_block(phi_block) {}
  PhiMergerCacheKey() : ptr(NULL), phi_block(NULL) {}

  bool operator==(const PhiMergerCacheKey& other) const {
    return ptr == other.ptr && phi_block == other.phi_block;
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
    return PhiMergerCacheKey(
      DenseMapInfo<const Value*>::getTombstoneKey(),
      DenseMapInfo<BasicBlock*>::getTombstoneKey()
    );
  }
  static unsigned getHashValue(const PhiMergerCacheKey &Val) {
    return DenseMapInfo<const Value*>::getHashValue(Val.ptr) ^
      DenseMapInfo<BasicBlock*>::getHashValue(Val.phi_block);
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

/// Merge a set of `PointerStatus`es for the given `ptr` in `phi_block`.
///
/// (See general comments on merging on `merge_direct`.)
///
/// If necessary, insert a phi instruction in `phi_block` to perform the
/// merge.
/// We will only potentially need to do this if at least one of the statuses
/// is `Dynamic` with a non-null `dynamic_kind`.
PointerStatus PointerStatus::merge_with_phi(
  const llvm::SmallVector<StatusWithBlock, 4>& statuses,
  const Value* ptr,
  llvm::BasicBlock* phi_block
) {
  bool have_dynamic_null = false;
  PointerStatus maybe_merged = notdefinedyet();
  for (const StatusWithBlock& swb : statuses) {
    maybe_merged = merge_maybe_dynamic(maybe_merged, swb.status);
    if (const Dynamic* dyn = std::get_if<Dynamic>(&swb.status.data)) {
      if (dyn->dynamic_kind == NULL) have_dynamic_null = true;
    }
  }
  // if the result of the merger is completely known statically, use that.
  // (We could theoretically use a PHI even in this case.
  // This would have the advantage of path-sensitive status instead of
  // a conservative static merger; but the disadvantage of having a dynamic
  // status (from phi) instead of a statically-known (merged) status.)
  if (!maybe_merged.is_dynamic()) return maybe_merged;
  // ok so we don't know the merge result statically
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
  // check the cache first to see if we've already created a phi for
  // the dynamic status of this `ptr` in this `phi_block`
  static DenseMap<PhiMergerCacheKey, PHINode*> cache;
  PhiMergerCacheKey Key(ptr, phi_block);
  bool have_cached_phi = cache.count(Key) > 0;
  // if we have a cached phi but it has the wrong number of inputs
  // (for instance, if we have added another incoming edge to the block)
  // then we'll create a completely fresh phi anyway.
  // `cached_phi` is nonnull iff we have a cached phi with the right number of
  // inputs.
  PHINode* cached_phi = NULL;
  if (have_cached_phi) {
    cached_phi = cache[Key];
    if (statuses.size() != cached_phi->getNumIncomingValues()) {
      cached_phi = NULL;
    }
  }
  if (cached_phi) {
    // check that it has the same incoming blocks as last time. If not, ignore
    // the cached phi (which will result in us making a new phi).
    for (const StatusWithBlock& swb : statuses) {
      if (cached_phi->getBasicBlockIndex(swb.block) < 0) {
        cached_phi = NULL;
        break;
      }
    }
  }
  if (cached_phi) {
    // Adjust the inputs to the phi if any incoming statuses have changed
    assert(statuses.size() == cached_phi->getNumIncomingValues());
    for (const StatusWithBlock& swb : statuses) {
      const Value* old_incoming_kind = cached_phi->getIncomingValueForBlock(swb.block);
      assert(old_incoming_kind && "should have already checked that all these blocks are valid");
      if (const Dynamic* dyn = std::get_if<Dynamic>(&swb.status.data)) {
        if (dyn->dynamic_kind == old_incoming_kind) {
          // nothing to update
        } else {
          // a different dynamic status than we had previously.
          // (or maybe we previously had a constant mask here.)
          // Update the incoming value in place.
          cached_phi->setIncomingValueForBlock(swb.block, dyn->dynamic_kind);
        }
      } else {
        ConstantInt* new_mask = swb.status.is_notdefinedyet() ?
          // for now, treat this input to the phi as `Clean`.
          // this will be updated on a future iteration if necessary, once the
          // incoming pointer has a defined status.
          // But if considering this `Clean` leads to the incoming pointer
          // eventually being `Clean`, that's fine, we're happy with that result
          ConstantInt::get(Type::getInt64Ty(swb.block->getContext()), Dynamic::Masks::clean)
          // incoming pointer status is defined, just get the appopriate dynamic
          // mask
          : swb.status.to_constant_dynamic_kind_mask(swb.block->getContext());
        if (const ConstantInt* old_mask = dyn_cast<const ConstantInt>(old_incoming_kind)) {
          if (new_mask->getValue() == old_mask->getValue()) {
            // nothing to update
          } else {
            // a different constant mask than we had previously.
            // Update the incoming value in place.
            cached_phi->setIncomingValueForBlock(swb.block, new_mask);
          }
        } else {
          // a constant mask, where previously we had a dynamic mask.
          // Update the incoming value in place.
          cached_phi->setIncomingValueForBlock(swb.block, new_mask);
        }
      }
    }
    cache[Key] = cached_phi; // shouldn't be necessary I suppose -- we just modified the `phi` in place
    return PointerStatus::dynamic(cached_phi);
  } else {
    // create a fresh phi
    DMSIRBuilder Builder(phi_block, DMSIRBuilder::PHIBEGINNING, NULL);
    std::string ptr_name = isa<ConstantExpr>(ptr) ? "constexpr" : ptr->getNameOrAsOperand();
    PHINode* phi = Builder.CreatePHI(Builder.getInt64Ty(), 2, Twine(ptr_name, "_dynamic_kind"));
    assert(statuses.size() == pred_size(phi_block));
    for (const StatusWithBlock& swb : statuses) {
      assert(block_is_pred_of_block(swb.block, phi_block));
      if (swb.status.is_notdefinedyet()) {
        // for now, treat this input to the phi as `Clean`.
        // this will be updated on a future iteration if necessary, once the
        // incoming pointer has a defined status.
        // But if considering this `Clean` leads to the incoming pointer
        // eventually being `Clean`, that's fine, we're happy with that result
        phi->addIncoming(
          Builder.getInt64(Dynamic::Masks::clean),
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

    cache[Key] = phi;
    return PointerStatus::dynamic(phi);
  }
}

/// Merge a static `PointerStatus` and a `dynamic_kind`, directly (inserting
/// dynamic instructions if necessary).
///
/// This function computes the merger without caching.
PointerStatus PointerStatus::merge_static_dynamic_nocache_direct(
  const PointerStatus static_status,
  Value* dynamic_kind,
  DMSIRBuilder& Builder
) {
  if (const NotDefinedYet* ndy = std::get_if<NotDefinedYet>(&static_status.data)) {
    // As in PointerStatus::merge_direct, merging x with `NotDefinedYet` is
    // always x
    (void)ndy; // silence warning about unused variable
    return dynamic(dynamic_kind);
  } else if (const Clean* clean = std::get_if<Clean>(&static_status.data)) {
    // For all x, merging `Clean` with x results in x
    (void)clean; // silence warning about unused variable
    return dynamic(dynamic_kind);
  } else if (const Blemished* blem = std::get_if<Blemished>(&static_status.data)) {
    if (blem->under16()) {
      // merging `Blemished`(<=16) with DYN_CLEAN or DYN_BLEMISHED16 is
      // DYN_BLEMISHED16.
      // merging `Blemished`(<=16) with any other x results in x.
      if (dynamic_kind == NULL) return dynamic(NULL);
      return dynamic(Builder.CreateSelect(
        Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(Dynamic::Masks::clean), "isclean"),
        Builder.getInt64(Dynamic::Masks::blemished16),
        dynamic_kind,
        "merged_dynamic_kind"
      ));
    } else {
      // in all other cases, merging any `Blemished` with DYN_CLEAN,
      // DYN_BLEMISHED16, or DYN_BLEMISHEDOTHER results in DYN_BLEMISHEDOTHER.
      // merging any `Blemished` with DYN_DIRTY results in DYN_DIRTY.
      if (dynamic_kind == NULL) return PointerStatus::dynamic(NULL);
      return PointerStatus::dynamic(Builder.CreateSelect(
        Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(Dynamic::Masks::dirty), "isdirty"),
        Builder.getInt64(Dynamic::Masks::dirty),
        Builder.getInt64(Dynamic::Masks::blemished_other),
        "merged_dynamic_kind"
      ));
    }
  } else if (const Dirty* d = std::get_if<Dirty>(&static_status.data)) {
    // merging anything with `Dirty` results in `Dirty`
    (void)d; // silence warning about unused variable
    return dirty();
  } else if (const Unknown* unk = std::get_if<Unknown>(&static_status.data)) {
    // merging anything with `Unknown` results in `Unknown`
    (void)unk; // silence warning about unused variable
    return unknown();
  } else if (static_status.is_dynamic()) {
    llvm_unreachable("merge_static_dynamic: expected a static PointerStatus");
  } else {
    llvm_unreachable("Missing PointerStatus case");
  }
}

/// Used only for the cache in `PointerStatus::merge_static_dynamic_direct`; see
/// notes there
struct StaticDynamicCacheKey {
  PointerStatus static_status;
  Value* dynamic_kind;
  BasicBlock* block;

  StaticDynamicCacheKey(
    PointerStatus static_status,
    Value* dynamic_kind,
    BasicBlock* block
  ) : static_status(static_status), dynamic_kind(dynamic_kind), block(block)
  {}

  bool operator==(const StaticDynamicCacheKey& other) const {
    return
      static_status == other.static_status &&
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
    return StaticDynamicCacheKey(PointerStatus::notdefinedyet(), NULL, NULL);
  }
  static inline StaticDynamicCacheKey getTombstoneKey() {
    return StaticDynamicCacheKey(
      PointerStatus::notdefinedyet(),
      DenseMapInfo<Value*>::getTombstoneKey(),
      DenseMapInfo<BasicBlock*>::getTombstoneKey()
    );
  }
  static unsigned getHashValue(const StaticDynamicCacheKey &Val) {
    return
      DenseMapInfo<Value*>::getHashValue(Val.dynamic_kind) ^
      DenseMapInfo<BasicBlock*>::getHashValue(Val.block) ^
      PointerStatus::getHashValue(Val.static_status);
  }
  static bool isEqual(const StaticDynamicCacheKey &LHS, const StaticDynamicCacheKey &RHS) {
    return LHS == RHS;
  }
};
} // end namespace llvm

/// Merge a static `PointerStatus` and a `dynamic_kind`.
/// See comments on PointerStatus::merge_direct.
///
/// This function performs caching. If these two things have been merged before,
/// in the same block (eg on a previous iteration), it returns the cached value;
/// else it computes the merger fresh.
PointerStatus PointerStatus::merge_static_dynamic_direct(
  const PointerStatus static_status,
  Value* dynamic_kind,
  DMSIRBuilder& Builder
) {
  static DenseMap<StaticDynamicCacheKey, PointerStatus> cache;
  StaticDynamicCacheKey Key(static_status, dynamic_kind, Builder.GetInsertBlock());
  if (cache.count(Key) > 0) return cache[Key];
  PointerStatus merged = merge_static_dynamic_nocache_direct(static_status, dynamic_kind, Builder);
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
  Value* dirty = Builder.getInt64(PointerStatus::Dynamic::Masks::dirty);
  Value* blemother = Builder.getInt64(PointerStatus::Dynamic::Masks::blemished_other);
  Value* blem16 = Builder.getInt64(PointerStatus::Dynamic::Masks::blemished16);
  Value* clean = Builder.getInt64(PointerStatus::Dynamic::Masks::clean);
  Value* either_is_dirty = Builder.CreateLogicalOr(
    Builder.CreateICmpEQ(dynamic_kind_a, dirty),
    Builder.CreateICmpEQ(dynamic_kind_b, dirty),
    "either_is_dirty"
  );
  Value* either_is_blemother = Builder.CreateLogicalOr(
    Builder.CreateICmpEQ(dynamic_kind_a, blemother),
    Builder.CreateICmpEQ(dynamic_kind_b, blemother),
    "either_is_blemother"
  );
  Value* either_is_blem16 = Builder.CreateLogicalOr(
    Builder.CreateICmpEQ(dynamic_kind_a, blem16),
    Builder.CreateICmpEQ(dynamic_kind_b, blem16),
    "either_is_blem16"
  );
  Value* merged_dynamic_kind =
    Builder.CreateSelect(either_is_dirty, dirty,
    Builder.CreateSelect(either_is_blemother, blemother,
    Builder.CreateSelect(either_is_blem16, blem16,
    clean)), "merged_dynamic_kind");
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
