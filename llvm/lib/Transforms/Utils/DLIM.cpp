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
    unsigned loads_clean_addr;
    // How many loads have a dirty pointer as address
    unsigned loads_dirty_addr;
    // How many stores have a clean pointer as address (we don't count the data
    // being stored, even if it's a pointer)
    unsigned stores_clean_addr;
    // How many stores have a dirty pointer as address (we don't count the data
    // being stored, even if it's a pointer)
    unsigned stores_dirty_addr;
    // How many times are we storing a clean pointer to memory (this doesn't
    // care whether the address of the store is clean or dirty)
    unsigned stores_clean_val;
    // How many times are we storing a dirty pointer to memory (this doesn't
    // care whether the address of the store is clean or dirty)
    unsigned stores_dirty_val;
    // How many times are we passing a clean pointer to a function
    unsigned passing_clean_ptr;
    // How many times are we passing a dirty pointer to a function
    unsigned passing_dirty_ptr;
    // How many times are we returning a clean pointer from a function
    unsigned returning_clean_ptr;
    // How many times are we returning a dirty pointer from a function
    unsigned returning_dirty_ptr;
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
    results = { 0 };

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
          case Instruction::Store: {
            const StoreInst& store = cast<StoreInst>(inst);
            // first count the stored value for stats purposes (if it's a pointer)
            const Value* storedVal = store.getValueOperand();
            if (storedVal->getType()->isPointerTy()) {
              if (clean_ptrs.contains(storedVal)) {
                results.stores_clean_val++;
              } else {
                results.stores_dirty_val++;
              }
            }
            // next count the address for stats purposes
            const Value* addr = store.getPointerOperand();
            if (clean_ptrs.contains(addr)) {
              results.stores_clean_addr++;
            } else {
              results.stores_dirty_addr++;
            }
            // now, the pointer used as an address becomes clean
            clean_ptrs.insert(addr);
            break;
          }
          case Instruction::Load: {
            const LoadInst& load = cast<LoadInst>(inst);
            const Value* ptr = load.getPointerOperand();
            // first count this for stats purposes
            if (clean_ptrs.contains(ptr)) {
              results.loads_clean_addr++;
            } else {
              results.loads_dirty_addr++;
            }
            // now, the pointer becomes clean
            clean_ptrs.insert(ptr);

            if (load.getType()->isPointerTy()) {
              // in this case, we loaded a pointer from memory, and have to
              // worry about whether it's clean or not.
              // For now, our assumption is it's clean.
              clean_ptrs.insert(&load);
            }
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
          case Instruction::Select: {
            const SelectInst& select = cast<SelectInst>(inst);
            if (select.getType()->isPointerTy()) {
              // output is clean if both inputs are clean
              const Value* true_input = select.getTrueValue();
              const Value* false_input = select.getFalseValue();
              if (clean_ptrs.contains(true_input) && clean_ptrs.contains(false_input)) {
                clean_ptrs.insert(&select);
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
          case Instruction::IntToPtr: {
            // inttoptr always produces a dirty result
            // so do nothing
            break;
          }
          case Instruction::Call: {
            const CallInst& call = cast<CallInst>(inst);
            // count call arguments for stats purposes
            for (const Use& arg : call.args()) {
              const Value* value = arg.get();
              if (value->getType()->isPointerTy()) {
                if (clean_ptrs.contains(value)) {
                  results.passing_clean_ptr++;
                } else {
                  results.passing_dirty_ptr++;
                }
              }
            }
            // For now, our assumption is that pointers returned from calls are clean
            clean_ptrs.insert(&call);
            break;
          }
          case Instruction::Ret: {
            const ReturnInst& ret = cast<ReturnInst>(inst);
            const Value* retval = ret.getReturnValue();
            if (retval && retval->getType()->isPointerTy()) {
              if (clean_ptrs.contains(retval)) {
                results.returning_clean_ptr++;
              } else {
                results.returning_dirty_ptr++;
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
  dbgs() << "Loads with clean addr: " << results.loads_clean_addr << "\n";
  dbgs() << "Loads with dirty addr: " << results.loads_dirty_addr << "\n";
  dbgs() << "Stores with clean addr: " << results.stores_clean_addr << "\n";
  dbgs() << "Stores with dirty addr: " << results.stores_dirty_addr << "\n";
  dbgs() << "Storing a clean ptr to mem: " << results.stores_clean_val << "\n";
  dbgs() << "Storing a dirty ptr to mem: " << results.stores_dirty_val << "\n";
  dbgs() << "Passing a clean ptr to a func: " << results.passing_clean_ptr << "\n";
  dbgs() << "Passing a dirty ptr to a func: " << results.passing_dirty_ptr << "\n";
  dbgs() << "Returning a clean ptr from a func: " << results.returning_clean_ptr << "\n";
  dbgs() << "Returning a dirty ptr from a func: " << results.returning_dirty_ptr << "\n";
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
