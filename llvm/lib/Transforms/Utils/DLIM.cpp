#include "llvm/Transforms/Utils/DLIM.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "DLIM"

// SmallDenseSet doesn't seem to have a set-equality operator, so for now we
// just implement a fairly naive one
template<typename T, unsigned N>
static bool setsAreEqual(const SmallDenseSet<T, N> &a, const SmallDenseSet<T, N> &b);

// SmallDenseSet doesn't seem to have a set-difference operator, so for now we
// just implement a fairly naive one.
// This removes from A, any items that appear in B.
template<typename T, unsigned N>
static void setDiff(SmallDenseSet<T, N> &a, const SmallDenseSet<T, N> &b);

// SmallDenseSet doesn't seem to have a set-intersection operator, so for now
// we just implement a fairly naive one
template<typename T, unsigned N>
static SmallDenseSet<T, N> setIntersect(const SmallDenseSet<T, N> &a, const SmallDenseSet<T, N> &b);

// True if all of the indices of the GEP are constant 0. False if any index is
// not constant 0.
static bool areAllGEPIndicesZero(const GetElementPtrInst &gep);

// Return a string describing the given set of clean ptrs (for debugging purposes)
template<unsigned N>
static std::string describeCleanPointers(const SmallDenseSet<const Value*, N> &clean_ptrs);

class DLIMAnalysis {
public:
  /// Creates and initializes the Analysis but doesn't actually run the analysis
  DLIMAnalysis(Function &F) : F(F) {
    initialize_block_states();
  }
  ~DLIMAnalysis() {}

  /// This struct holds the results of the analysis
  typedef struct Results {
    // How many loads have a clean pointer as address
    unsigned clean_loads;
    // How many loads have a dirty pointer as address
    unsigned dirty_loads;
    // How many stores have a clean pointer as address (we don't count the data
    // being stored, even if it's a pointer)
    unsigned clean_stores;
    // How many stores have a dirty pointer as address (we don't count the data
    // being stored, even if it's a pointer)
    unsigned dirty_stores;
  } Results;

  /// Runs the analysis and returns the `Results`
  Results run() {
    Results results;

    bool changed = true;
    while (changed) {
      LLVM_DEBUG(dbgs() << "DLIM: starting an iteration\n");
      changed = this->doIteration(results);
    }

    return results;
  }

private:
  Function &F;

  /// This struct holds the per-block state for the analysis
  typedef struct PerBlockState {
    /// The _clean_ pointers at the _beginning_ of the block.
    /// Note that all pointers are assumed dirty until proven clean.
    SmallDenseSet<const Value*, 8> clean_ptrs_beg;
    /// The _clean_ pointers at the _end_ of the block.
    /// Note that all pointers are assumed dirty until proven clean.
    SmallDenseSet<const Value*, 8> clean_ptrs_end;
  } PerBlockState;

  DenseMap<const BasicBlock*, PerBlockState> block_states;

  void initialize_block_states() {
    for (const BasicBlock &block : F) {
      PerBlockState pbs = PerBlockState {
        SmallDenseSet<const Value*, 8>(),
        SmallDenseSet<const Value*, 8>(),
      };
      block_states.insert(
        std::pair<const BasicBlock*, PerBlockState>(&block, std::move(pbs))
      );
    }

    // also, per our current assumptions, if any function parameters are
    // pointers, mark them clean in the function's entry block
    PerBlockState& entry_block_pbs = block_states.find(&F.getEntryBlock())->getSecond();
    for (const Argument& arg : F.args()) {
      if (arg.getType()->isPointerTy()) {
        entry_block_pbs.clean_ptrs_beg.insert(&arg);
      }
    }
  }

  /// Returns `true` if any change was made to internal state
  bool doIteration(Results &results) {
    // Reset the results - we'll collect them new
    results = { 0, 0, 0, 0 };

    bool changed = false;

    for (const BasicBlock &block : F) {
      PerBlockState& pbs = block_states.find(&block)->getSecond();
      LLVM_DEBUG(dbgs() << "DLIM: analyzing block which previously had " << pbs.clean_ptrs_beg.size() << " clean ptrs at beginning and " << pbs.clean_ptrs_end.size() << " clean ptrs at end\n");

      // first: if any variable is clean at the end of all of this block's
      // predecessors, then it is also clean at the beginning of this block
      if (!block.hasNPredecessors(0)) {
        auto preds = pred_begin(&block);
        const BasicBlock* firstPred = *preds;
        const PerBlockState& firstPred_pbs = block_states.find(firstPred)->getSecond();
        // we start with all of the clean_ptrs at the end of our first predecessor,
        // then remove any of them that aren't clean at the end of other predecessors
        SmallDenseSet<const Value*, 8> clean_ptrs =
          SmallDenseSet<const Value*, 8>(firstPred_pbs.clean_ptrs_end.begin(), firstPred_pbs.clean_ptrs_end.end());
        LLVM_DEBUG(dbgs() << "DLIM:   first predecessor has " << describeCleanPointers(clean_ptrs) << " at end\n");
        for (auto it = ++preds, end = pred_end(&block); it != end; ++it) {
          const BasicBlock* otherPred = *it;
          const PerBlockState& otherPred_pbs = block_states.find(otherPred)->getSecond();
          LLVM_DEBUG(dbgs() << "DLIM:   next predecessor has " << describeCleanPointers(otherPred_pbs.clean_ptrs_end) << " at end\n");
          clean_ptrs = setIntersect(std::move(clean_ptrs), otherPred_pbs.clean_ptrs_end);
        }
        // whatever's left is now the set of clean ptrs at beginning of this block
        changed |= !setsAreEqual(pbs.clean_ptrs_beg, clean_ptrs);
        pbs.clean_ptrs_beg = std::move(clean_ptrs);
      }
      LLVM_DEBUG(dbgs() << "DLIM:   at beginning of block, we now have " << describeCleanPointers(pbs.clean_ptrs_beg) << "\n");

      // The pointers which are currently clean. This begins as
      // `pbs.clean_ptrs_beg`, and as we go through the block, gets
      // updated; its state at the end of the block will become
      // `pbs.clean_ptrs_end`.
      SmallDenseSet<const Value*, 8> clean_ptrs =
        SmallDenseSet<const Value*, 8>(pbs.clean_ptrs_beg.begin(), pbs.clean_ptrs_beg.end());

      // now: process each instruction
      // the only way for a dirty pointer to become clean is by being dereferenced
      // there is no way for a clean pointer to become dirty
      // so we only need to worry about pointer dereferences, and instructions
      // which produce clean pointers
      // (and of course we want to count clean/dirty loads/stores)
      for (const Instruction &inst : block) {
        switch (inst.getOpcode()) {
          case Instruction::Load: {
            const LoadInst& load = cast<LoadInst>(inst);
            const Value* ptr = load.getPointerOperand();
            // first count this for stats purposes
            if (clean_ptrs.contains(ptr)) {
              results.clean_loads++;
            } else {
              results.dirty_loads++;
            }
            // now, the pointer becomes clean
            clean_ptrs.insert(ptr);
            break;
          }
          case Instruction::Store: {
            const StoreInst& store = cast<StoreInst>(inst);
            const Value* ptr = store.getPointerOperand();
            // first count this for stats purposes
            if (clean_ptrs.contains(ptr)) {
              results.clean_stores++;
            } else {
              results.dirty_stores++;
            }
            // now, the pointer becomes clean
            clean_ptrs.insert(ptr);
            break;
          }
          case Instruction::Alloca: {
            // result of an alloca is a clean pointer
            clean_ptrs.insert(&inst);
            break;
          }
          case Instruction::GetElementPtr: {
            const GetElementPtrInst& gep = cast<GetElementPtrInst>(inst);
            if (areAllGEPIndicesZero(gep)) {
              // result of a GEP with all zeroes as indices, is the same as the input pointer.
              const Value* input_ptr = gep.getPointerOperand();
              if (clean_ptrs.contains(input_ptr)) {
                clean_ptrs.insert(&gep);
              }
            } else {
              // result of a GEP with any nonzero indices is a dirty pointer.
              // (do nothing - the result is dirty by default, by virtue of not
              // being included in `clean_ptrs`)
            }
            break;
          }
          case Instruction::BitCast: {
            const BitCastInst& bitcast = cast<BitCastInst>(inst);
            if (bitcast.getType()->isPointerTy()) {
              const Value* input_ptr = bitcast.getOperand(0);
              if (clean_ptrs.contains(input_ptr)) {
                clean_ptrs.insert(&bitcast);
              }
            }
            break;
          }
          case Instruction::PHI: {
            const PHINode& phi = cast<PHINode>(inst);
            if (phi.getType()->isPointerTy()) {
              // phi: if all inputs are clean in their corresponding blocks, result is clean
              // else result is dirty
              bool all_clean = true;
              for (const Use& use : phi.incoming_values()) {
                const BasicBlock* bb = phi.getIncomingBlock(use);
                auto& clean_ptrs_end_of_bb = block_states.find(bb)->getSecond().clean_ptrs_end;
                const Value* value = use.get();
                if (!clean_ptrs_end_of_bb.contains(value)) {
                  all_clean = false;
                }
              }
              if (all_clean) {
                clean_ptrs.insert(&phi);
              }
            }
            break;
          }
          default:
            if (inst.getType()->isPointerTy()) {
              errs() << "Encountered a pointer-producing instruction which we don't have a case for. Does it produce a clean or dirty pointer?\n";
              inst.dump();
            }
            break;
        }
      }

      // Now that we've processed all the instructions, we have the final list
      // of clean pointers as of the end of the block
      LLVM_DEBUG(dbgs() << "DLIM:   at end of block, we now have " << describeCleanPointers(clean_ptrs) << "\n");
      changed |= !setsAreEqual(pbs.clean_ptrs_end, clean_ptrs);
      pbs.clean_ptrs_end = std::move(clean_ptrs);
    }

    return changed;
  }
};

PreservedAnalyses DLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis::Results results = DLIMAnalysis(F).run();

  dbgs() << F.getName() << ":\n";
  dbgs() << "Clean loads: " << results.clean_loads << "\n";
  dbgs() << "Dirty loads: " << results.dirty_loads << "\n";
  dbgs() << "Clean stores: " << results.clean_stores << "\n";
  dbgs() << "Dirty stores: " << results.dirty_stores << "\n";
  dbgs() << "\n";

  // Right now, the pass only analyzes the IR and doesn't make any changes, so
  // all analyses are preserved
  return PreservedAnalyses::all();
}

// SmallDenseSet doesn't seem to have a set-equality operator, so for now we
// just implement a fairly naive one
template<typename T, unsigned N>
static bool setsAreEqual(const SmallDenseSet<T, N> &a, const SmallDenseSet<T, N> &b) {
  // fast case: first check the sizes, and if they aren't equal, we can exit early
  if (a.size() != b.size()) {
    return false;
  }
  // for sets of the same size, they're equal if A is a subset of B (no need to
  // check the reverse)
  for (const auto &item : a) {
    if (!b.contains(item)) {
      return false;
    }
  }
  return true;
}

// SmallDenseSet doesn't seem to have a set-difference operator, so for now we
// just implement a fairly naive one.
// This removes from A, any items that appear in B.
template<typename T, unsigned N>
static void setDiff(SmallDenseSet<T, N> &a, const SmallDenseSet<T, N> &b) {
  // TODO: SmallDenseSet has a `.erase()` that takes a `ConstIterator`.  Use that instead
  for (const auto &item : b) {
    a.erase(item);
  }
}

// SmallDenseSet doesn't seem to have a set-intersection operator, so for now
// we just implement a fairly naive one
template<typename T, unsigned N>
static SmallDenseSet<T, N> setIntersect(const SmallDenseSet<T, N> &a, const SmallDenseSet<T, N> &b) {
  SmallDenseSet<T, N> intersection;
  for (const auto &item : a) {
    if (b.contains(item)) {
      intersection.insert(item);
    }
  }
  return intersection;
}

// True if all of the indices of the GEP are constant 0. False if any index is
// not constant 0.
static bool areAllGEPIndicesZero(const GetElementPtrInst &gep) {
  for (const Use& idx : gep.indices()) {
    if (const ConstantInt* c = dyn_cast<ConstantInt>(idx.get())) {
      if (c->isZero()) {
        continue;
      } else {
        return false;
      }
    } else {
      return false;
    }
  }
  return true;
}

#include <sstream>  // ostringstream

// Return a string describing the given set of clean ptrs (for debugging purposes)
template<unsigned N>
static std::string describeCleanPointers(const SmallDenseSet<const Value*, N> &clean_ptrs) {
  switch (clean_ptrs.size()) {
    case 0: return "0 clean ptrs";
    case 1: {
      const Value* clean_ptr = *clean_ptrs.begin();
      std::ostringstream out;
      out << "1 clean ptr (" << clean_ptr->getNameOrAsOperand() << ")";
      return out.str();
    }
    default: {
      std::ostringstream out;
      out << clean_ptrs.size() << " clean ptrs (";
      for (const Value* clean_ptr : clean_ptrs) {
        out << clean_ptr->getNameOrAsOperand() << ", ";
      }
      out << ")";
      return out.str();
    }
  }
}
