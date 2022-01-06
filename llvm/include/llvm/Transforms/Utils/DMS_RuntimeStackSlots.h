#pragma once

#include "llvm/ADT/DenseSet.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

/// Stack slots used in this LLVM function for receiving output values from
/// the DMS runtime functions `__dms_get_bounds()` and
/// `__dms_get_globalarraysize()`.
///
/// These can be shared by all users in the LLVM function -- they are only
/// ever used very briefly, around one callsite at a time, so we don't ever
/// have to worry about the data being clobbered. One set of stack slots is
/// enough.
class RuntimeStackSlots {
  private:
  /// Stack slot for the `output_base` parameter to `__dms_get_bounds()`
  ///
  /// will be NULL until first demanded, for the given function
  llvm::AllocaInst* output_base;

  /// Stack slot for the `output_max` parameter to `__dms_get_bounds()` or
  /// `__dms_get_globalarraysize()`
  ///
  /// will be NULL until first demanded, for the given function
  llvm::AllocaInst* output_max;

  /// Reference to the Function where these stack slots live
  llvm::Function& F;
  /// Reference to the `added_insts` where we note any instructions added for
  /// bounds purposes. See notes on `added_insts` in `DMSAnalysis`
  llvm::DenseSet<const llvm::Instruction*>& added_insts;

  public:
  RuntimeStackSlots(llvm::Function& F, llvm::DenseSet<const llvm::Instruction*>& added_insts)
    : output_base(NULL), output_max(NULL), F(F), added_insts(added_insts) {}

  /// Get the output_base, initializing it first if necessary.
  ///
  /// This never returns NULL.
  llvm::AllocaInst* getOutputBase();
  /// Get the output_max, initializing it first if necessary.
  ///
  /// This never returns NULL.
  llvm::AllocaInst* getOutputMax();
};
