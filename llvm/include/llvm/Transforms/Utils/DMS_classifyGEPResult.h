#pragma once

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/DMS_PointerStatus.h"

#include <optional>

namespace llvm {

/// Describes the classification of a GEP result, as determined by
/// `classifyGEPResult()`.
struct GEPResultClassification {
  /// Classification of the result of the given `gep`.
  PointerStatus classification;

  /// If the GEP's total offset is a compile-time constant, this holds the
  /// value of the constant offset, in bytes.
  /// Otherwise, this is empty.
  /// (If `override_constant_offset` is non-NULL, this will simply reflect the
  /// values specified in the override.)
  std::optional<APInt> constant_offset;

  /// If `offset` is constant but nonzero, do we consider it as zero anyways
  /// because it is a "trustworthy" struct offset?
  bool trustworthy_struct_offset;

  std::string pretty() const;
};

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
);

} // end namespace llvm
