#include "llvm/ADT/APInt.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"

namespace llvm {

/// Return type for `isOffsetAnInductionPattern`.
///
/// If the offset of the given GEP is an induction pattern, then
/// `is_induction_pattern` will be `true`; the GEP has effectively the offset
/// `initial_offset` during the first loop iteration, and the offset is
/// incremented by `induction_offset` each subsequent loop iteration.
/// Offsets are in bytes.
///
/// If the offset is not an induction pattern, then `is_induction_pattern` will
/// be `false`, and the other fields are undefined.
struct InductionPatternResult {
  bool is_induction_pattern;
  APInt induction_offset;
  APInt initial_offset;
};

/// Is the offset of the given GEP an induction pattern?
/// This is looking for a pretty specific pattern for GEPs inside loops, which
/// we can optimize checks for.
InductionPatternResult isOffsetAnInductionPattern(
	const GetElementPtrInst&,
	const DataLayout&,
	const LoopInfo&,
	const PostDominatorTree&
);

} // end namespace
