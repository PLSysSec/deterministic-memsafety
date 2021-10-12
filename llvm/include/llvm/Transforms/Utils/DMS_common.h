// Struct definitions and utility functions needed by multiple DMS files

#ifndef DMS_COMMON_H
#define DMS_COMMON_H

#include "llvm/ADT/APInt.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/DMS_PointerStatus.h"

/// Struct exists because we can't use C++17's std::optional.
/// Describes whether a call allocates memory, and if so, the size of
/// the memory allocated.
struct IsAllocatingCall {
  /// Is it an allocating call
  bool is_allocating;
  /// If it's an allocating call, this is the allocation size in bytes.
  /// If it's not an allocating call, this field is invalid (and may be NULL).
  llvm::Value* allocation_bytes;

  static IsAllocatingCall not_allocating() { return IsAllocatingCall { false, NULL }; }
  static IsAllocatingCall allocating(llvm::Value* size) { return IsAllocatingCall { true, size }; }
};

/// Describes the classification of a GEP result, as determined by
/// `classifyGEPResult()`.
struct GEPResultClassification {
  /// Classification of the result of the given `gep`.
  PointerStatus classification;
  /// Was the total offset of the GEP considered a constant?
  /// (If `override_constant_offset` is non-NULL, this will always be `true`, of
  /// course.)
  bool offset_is_constant;
  /// If `offset_is_constant` is `true`, then this holds the value of the
  /// constant offset, in _bytes_.
  /// (If `override_constant_offset` is non-NULL, `classifyGEPResult` will
  /// copy that value to `constant_offset`.)
  llvm::APInt constant_offset;
  /// If `constant_offset` is nonzero, do we consider it as zero anyways because
  /// it is a "trustworthy" struct offset?
  bool trustworthy_struct_offset;
};

#endif
