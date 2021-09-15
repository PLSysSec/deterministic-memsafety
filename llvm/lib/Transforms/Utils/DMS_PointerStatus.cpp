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
/// `insertion_pt`: If we need to insert new dynamic instructions to handle
/// a dynamic merge, insert them before this Instruction.
/// We will only potentially need to do this if at least one of the statuses
/// is DYNAMIC with a non-null `dynamic_kind`. If neither of the statuses is
/// DYNAMIC with a non-null `dynamic_kind`, then this parameter is ignored
/// (and may be NULL).
PointerStatus PointerStatus::merge(const PointerStatus a, const PointerStatus b, Instruction* insertion_pt) {
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
Value* PointerStatus::to_dynamic_kind_mask(IRBuilder<>& Builder) const {
  switch (kind) {
    case PointerKind::CLEAN:
      return Builder.getInt64(DynamicKindMasks::clean);
      break;
    case PointerKind::BLEMISHED16:
      return Builder.getInt64(DynamicKindMasks::blemished16);
      break;
    case PointerKind::BLEMISHED32:
    case PointerKind::BLEMISHED64:
    case PointerKind::BLEMISHEDCONST:
      return Builder.getInt64(DynamicKindMasks::blemished_other);
      break;
    case PointerKind::DIRTY:
      return Builder.getInt64(DynamicKindMasks::dirty);
      break;
    case PointerKind::UNKNOWN:
      // for now we just mark UNKNOWN pointers as dirty when storing them
      return Builder.getInt64(DynamicKindMasks::dirty);
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

/// Merge a static `PointerKind` and a `dynamic_kind`.
/// See comments on PointerStatus::merge.
PointerStatus PointerStatus::merge_static_dynamic(const PointerKind static_kind, Value* dynamic_kind, Instruction* insertion_pt) {
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
        Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(DynamicKindMasks::clean)),
        Builder.getInt64(DynamicKindMasks::blemished16),
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
        Builder.CreateICmpEQ(dynamic_kind, Builder.getInt64(DynamicKindMasks::dirty)),
        Builder.getInt64(DynamicKindMasks::dirty),
        Builder.getInt64(DynamicKindMasks::blemished_other)
      );
      break;
    case PointerKind::DIRTY:
    case PointerKind::UNKNOWN:
      // merging anything with DIRTY or UNKNOWN results in DYN_DIRTY
      merged_dynamic_kind = Builder.getInt64(DynamicKindMasks::dirty);
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
Value* PointerStatus::merge_two_dynamic(Value* dynamic_kind_a, Value* dynamic_kind_b, Instruction* insertion_pt) {
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
