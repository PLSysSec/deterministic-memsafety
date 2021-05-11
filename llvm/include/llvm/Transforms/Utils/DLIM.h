#ifndef LLVM_TRANSFORMS_UTILS_DLIM_H
#define LLVM_TRANSFORMS_UTILS_DLIM_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class DLIMPass : public PassInfoMixin<DLIMPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

/// As of this writing, the only difference between `ParanoidDLIMPass`
/// and `DLIMPass` is that `ParanoidDLIMPass` doesn't trust LLVM struct
/// types (i.e., doesn't trust that the target code's pointer-type casts were
/// done correctly)
class ParanoidDLIMPass : public PassInfoMixin<DLIMPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  // This pass must run even on -O0
  static bool isRequired() { return true; }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_DLIM_H
