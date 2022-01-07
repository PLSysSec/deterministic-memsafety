#ifndef LLVM_TRANSFORMS_UTILS_DMS_H
#define LLVM_TRANSFORMS_UTILS_DMS_H

#include "llvm/IR/PassManager.h"

namespace llvm {

/// Pass which prints static counts of clean/dirty pointers and some other
/// statistics
class StaticDMSPass : public PassInfoMixin<StaticDMSPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// As of this writing, the differences between `ParanoidStaticDMSPass` and
/// `StaticDMSPass` are:
///   - `ParanoidStaticDMSPass` doesn't trust LLVM struct types (i.e., doesn't
///   trust that the target code's pointer-type casts were done correctly)
///   - `ParanoidStaticDMSPass` treats `inttoptr` results (integer-to-pointer
///   casts) as DIRTY, while `StaticDMSPass` assumes they are CLEAN
class ParanoidStaticDMSPass : public PassInfoMixin<ParanoidStaticDMSPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// Pass which instruments code so that it tracks dynamic counts of clean/dirty
/// pointers and some other statistics; then logs them to a file in
/// `./dms_dynamic_counts` in the runtime current directory.
class DynamicDMSPass : public PassInfoMixin<DynamicDMSPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// Like `DynamicDMSPass`, but the dynamic statistics are printed to stdout
/// when the program is run, rather than being logged to a file.
class DynamicStdoutDMSPass : public PassInfoMixin<DynamicStdoutDMSPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// Pass which adds SW bounds checks for all dereferences of pointers that
/// aren't CLEAN or BLEMISHED16. This uses the "non-paranoid" pointer statuses.
class BoundsChecksDMSPass : public PassInfoMixin<BoundsChecksDMSPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// This module pass is also required when we're adding SW bounds checks.
class BoundsChecksModuleDMSPass : public PassInfoMixin<BoundsChecksModuleDMSPass> {
public:
	PreservedAnalyses run(Module &mod, ModuleAnalysisManager &MAM);

	// This pass must run even on -O0
	static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_DMS_H
