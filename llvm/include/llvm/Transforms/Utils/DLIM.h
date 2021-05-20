#ifndef LLVM_TRANSFORMS_UTILS_DLIM_H
#define LLVM_TRANSFORMS_UTILS_DLIM_H

#include "llvm/IR/PassManager.h"

namespace llvm {

/// Pass which prints static counts of clean/dirty pointers and some other
/// statistics
class StaticDLIMPass : public PassInfoMixin<StaticDLIMPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// As of this writing, the only difference between `ParanoidStaticDLIMPass` and
/// `StaticDLIMPass` is that `ParanoidStaticDLIMPass` doesn't trust LLVM struct
/// types (i.e., doesn't trust that the target code's pointer-type casts were
/// done correctly)
class ParanoidStaticDLIMPass : public PassInfoMixin<ParanoidStaticDLIMPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// Pass which instruments code so that it tracks dynamic counts of clean/dirty
/// pointers and some other statistics; then logs them to a file in
/// `./dlim_dynamic_counts` in the runtime current directory.
class DynamicDLIMPass : public PassInfoMixin<DynamicDLIMPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// Like `DynamicDLIMPass`, but the dynamic statistics are printed to stdout
/// when the program is run, rather than being logged to a file.
class DynamicStdoutDLIMPass : public PassInfoMixin<DynamicStdoutDLIMPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_DLIM_H
