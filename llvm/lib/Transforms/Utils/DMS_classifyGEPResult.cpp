#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/DMS_classifyGEPResult.h"
#include "llvm/Transforms/Utils/DMS_common.h"

#include <sstream>  // ostringstream

using namespace llvm;

static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep);

std::string GEPResultClassification::pretty() const {
  std::ostringstream out;
  out << classification.pretty();

  if (constant_offset.has_value()) {
    llvm::SmallVector<char> offset_str;
    constant_offset->toStringSigned(offset_str);
    out << " with constant offset " << Twine(offset_str).str();
  }
  else out << " with nonconstant offset";

  if (trustworthy_struct_offset) out << " (considered zero due to trustworthy_struct_offset)";
  return out.str();
}

/// Classify the `PointerKind` of the result of the given `gep`, assuming that its
/// input pointer is `input_kind`.
/// This looks only at the `GetElementPtrInst` itself, and thus does not try to
/// do any loop induction reasoning etc (that is done elsewhere).
/// Think of this as giving the raw/default result for the `gep`.
///
/// `override_constant_offset`: if this is not NULL, then ignore the GEP's indices
/// and classify it as if the offset were the given compile-time constant.
static GEPResultClassification classifyGEPResult_direct(
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
    grc.constant_offset = computeGEPOffset(gep, DL);
  } else {
    grc.constant_offset = *override_constant_offset;
  }

  if (gep.hasAllZeroIndices()) {
    // result of a GEP with all zeroes as indices, is the same as the input pointer.
    assert(grc.constant_offset.value() == 0 && "If all indices are constant 0, then the total offset should be constant 0");
    grc.classification = input_status;
    grc.trustworthy_struct_offset = false;
    return grc;
  }
  if (trust_llvm_struct_types && areAllIndicesTrustworthy(gep)) {
    // nonzero offset, but "trustworthy" offset.
    assert(grc.constant_offset.has_value());
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
          std::string gep_name = isa<ConstantExpr>(gep) ? "constgep" : gep.getNameOrAsOperand();
          Value* dynamic_kind = Builder.CreateSelect(
            Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16), Twine(gep_name, "_input_blem16")),
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
  grc.trustworthy_struct_offset = false;
  if (grc.constant_offset.has_value()) {
    switch (input_status.kind) {
      case PointerKind::CLEAN: {
        // This GEP adds a constant but nonzero amount to a CLEAN
        // pointer. The result is some flavor of BLEMISHED depending
        // on how far the pointer arithmetic goes.
        if (grc.constant_offset->ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          grc.classification = PointerStatus::blemished16();
          return grc;
        } else if (grc.constant_offset->ule(APInt(/* bits = */ 64, /* val = */ 32))) {
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (grc.constant_offset->ule(APInt(/* bits = */ 64, /* val = */ 64))) {
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
        if (grc.constant_offset->ule(APInt(/* bits = */ 64, /* val = */ 16))) {
          // Conservatively, the total offset can't exceed 32
          grc.classification = PointerStatus::blemished32();
          return grc;
        } else if (grc.constant_offset->ule(APInt(/* bits = */ 64, /* val = */ 48))) {
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
        if (grc.constant_offset->ule(APInt(/* bits = */ 64, /* val = */ 32))) {
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
          std::string gep_name = isa<ConstantExpr>(gep) ? "constgep" : gep.getNameOrAsOperand();
          Value* is_clean = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::clean), Twine(gep_name, "_input_clean"));
          Value* is_blem16 = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished16), Twine(gep_name, "_input_blem16"));
          Value* is_blemother = Builder.CreateICmpEQ(input_status.dynamic_kind, Builder.getInt64(DynamicKindMasks::blemished_other), Twine(gep_name, "_input_blemother"));
          Value* dynamic_kind = Builder.getInt64(DynamicKindMasks::dirty);
          dynamic_kind = Builder.CreateSelect(
            is_clean,
            (grc.constant_offset->ule(APInt(/* bits = */ 64, /* val = */ 16))) ?
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

/// Same as `classifyGEPResult_direct`, but caches its results. If you call this
/// with the same arguments multiple times, you'll get the same result back.
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
) {
  static DenseMap<GEPResultCacheKey, GEPResultClassification> cache;
  GEPResultCacheKey key(&gep, input_status, trust_llvm_struct_types, override_constant_offset);
  if (cache.count(key) > 0) return cache[key];
  GEPResultClassification res = classifyGEPResult_direct(gep, input_status, DL, trust_llvm_struct_types, override_constant_offset, added_insts);
  cache[key] = res;
  return res;
}

namespace llvm {

/// Classify the `PointerStatus` of the result of the given `gep`, assuming that its
/// input pointer is `input_status`.
/// This looks only at the `GetElementPtrInst` itself, and thus does not try to
/// do any loop induction reasoning etc (that is done elsewhere).
/// Think of this as giving the raw/default result for the `gep`.
///
/// `override_constant_offset`: if this is not NULL, then ignore the GEP's indices
/// and classify it as if the offset were the given compile-time constant.
GEPResultClassification classifyGEPResult(
  GetElementPtrInst &gep,
  const PointerStatus input_status,
  const DataLayout &DL,
  const bool trust_llvm_struct_types,
  const APInt* override_constant_offset,
  DenseSet<const Instruction*>& added_insts
) {
  return classifyGEPResult_cached(
    gep,
    input_status,
    DL,
    trust_llvm_struct_types,
    override_constant_offset,
    added_insts
  );
}

} // end namespace llvm
