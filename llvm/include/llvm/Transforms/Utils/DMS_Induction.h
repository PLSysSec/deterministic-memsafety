#include "llvm/ADT/APInt.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"

#include <optional>

namespace llvm {

/// Return type for `isOffsetAnInductionPattern`.
///
/// If the offset of the given GEP is an induction pattern, then the GEP has
/// effectively the offset `initial_offset` during the first loop iteration, and
/// the offset is incremented by `induction_offset` each subsequent loop
/// iteration.
/// Offsets are in bytes.
struct InductionPattern {
  APInt induction_offset;
  APInt initial_offset;
};

/// Is the offset of the given GEP an induction pattern?
/// This is looking for a pretty specific pattern for GEPs inside loops, which
/// we can optimize checks for.
///
/// If so, return the `InductionPattern`; else, return nothing.
std::optional<InductionPattern> isOffsetAnInductionPattern(
	const GetElementPtrInst&,
	const DataLayout&,
	const LoopInfo&,
	const PostDominatorTree&
);

} // end namespace
