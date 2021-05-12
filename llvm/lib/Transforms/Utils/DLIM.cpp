#include "llvm/Transforms/Utils/DLIM.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"

#include <sstream>  // ostringstream

using namespace llvm;

#define DEBUG_TYPE "DLIM"

static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep);

typedef enum PointerKind {
  // As of this writing, the operations producing UNKNOWN are: loading a pointer
  // from memory; returning a pointer from a call; and receiving a pointer as a
  // function parameter
  UNKNOWN = 0,
  // CLEAN means "not modified since last allocated or dereferenced"
  CLEAN,
  // BLEMISHED means "incremented by BLEMISHED_BYTES or less from a clean
  // pointer"
  BLEMISHED,
  // DIRTY means "may have been incremented by more than BLEMISHED_BYTES (or
  // decremented by any amount) since last allocated or dereferenced"
  DIRTY,
} PointerKind;

/// How many bytes can a clean pointer be incremented by and still be considered
/// "blemished" rather than "dirty"
static const int BLEMISHED_BYTES = 16;

/// Merge two `PointerKind`s.
/// For the purposes of this function, the ordering is
/// DIRTY < UNKNOWN < BLEMISHED < CLEAN, and the merge returns the least element.
static PointerKind merge(const PointerKind a, const PointerKind b) {
  if (a == DIRTY || b == DIRTY) {
    return DIRTY;
  } else if (a == UNKNOWN || b == UNKNOWN) {
    return UNKNOWN;
  } else if (a == BLEMISHED || b == BLEMISHED) {
    return BLEMISHED;
  } else if (a == CLEAN && b == CLEAN) {
    return CLEAN;
  } else {
    assert(false && "Missing case in merge function");
  }
}

/// Conceptually stores the PointerKind of all currently valid pointers at a
/// particular program point.
class PointerStatuses {
public:
  PointerStatuses() {}
  ~PointerStatuses() {}

  void mark_clean(const Value* ptr) {
    mark_as(ptr, CLEAN);
  }

  void mark_dirty(const Value* ptr) {
    mark_as(ptr, DIRTY);
  }

  void mark_blemished(const Value* ptr) {
    mark_as(ptr, BLEMISHED);
  }

  void mark_unknown(const Value* ptr) {
    mark_as(ptr, UNKNOWN);
  }

  void mark_as(const Value* ptr, PointerKind kind) {
    // insert() does nothing if the key was already in the map.
    // instead, it appears we have to use operator[], which seems to
    // work whether or not `ptr` was already in the map
    map[ptr] = kind;
  }

  PointerKind getStatus(const Value* ptr) {
    auto it = map.find(ptr);
    if (it == map.end()) {
      mark_dirty(ptr);
      return DIRTY;
    } else {
      return it->getSecond();
    }
  }

  size_t numClean() const {
    unsigned count = 0;
    for (auto& pair : map) {
      if (pair.getSecond() == CLEAN) {
        count++;
      }
    }
    return count;
  }

  bool isEqualTo(const PointerStatuses& other) const {
    // first we check if every pair in A is also in B
    for (const auto &pair : map) {
      const auto& it = other.map.find(pair.getFirst());
      if (it == other.map.end()) {
        // key wasn't in other.map
        if (pair.getSecond() == DIRTY) {
          // missing from other.map, but marked DIRTY in this map: the maps are
          // still equivalent (missing is implicitly DIRTY)
          continue;
        } else {
          // missing from other.map, but non-DIRTY in this map
          return false;
        }
      }
      if (it->getSecond() != pair.getSecond()) {
        // maps disagree on what this key maps to
        return false;
      }
    }
    // now we check if every pair in B is also in A
    for (const auto &pair : other.map) {
      const auto &it = map.find(pair.getFirst());
      if (it == map.end()) {
        // key wasn't in this map
        if (pair.getSecond() == DIRTY) {
          // missing from this map, but marked DIRTY in other.map: the maps are
          // still equivalent (missing is implicitly DIRTY)
          continue;
        } else {
          // missing from this map, but non-DIRTY in other.map
          return false;
        }
      }
      // if the key is in both maps, we already checked above that the maps
      // agree on what this key maps to
    }
    return true;
  }

  std::string describe() const {
    SmallVector<const Value*, 8> clean_ptrs = SmallVector<const Value*, 8>();
    SmallVector<const Value*, 8> unk_ptrs = SmallVector<const Value*, 8>();
    for (auto& pair : map) {
      if (pair.getSecond() == CLEAN) {
        clean_ptrs.push_back(pair.getFirst());
      } else if (pair.getSecond() == UNKNOWN) {
        unk_ptrs.push_back(pair.getFirst());
      }
    }
    std::ostringstream out;
    switch (clean_ptrs.size()) {
      case 0: {
        out << "0 clean ptrs";
        break;
      }
      case 1: {
        const Value* clean_ptr = clean_ptrs[0];
        out << "1 clean ptr (" << clean_ptr->getNameOrAsOperand() << ")";
        break;
      }
      default: {
        out << clean_ptrs.size() << " clean ptrs (";
        for (const Value* clean_ptr : clean_ptrs) {
          out << clean_ptr->getNameOrAsOperand() << ", ";
        }
        out << ")";
        break;
      }
    }
    out << " and " << unk_ptrs.size() << " unknown ptrs";
    return out.str();
  }

  /// Merge the two given PointerStatuses. If they disagree on any pointer,
  /// use the `PointerKind` `merge` function to combine the two results.
  /// Recall that any pointer not appearing in the `map` is considered DIRTY.
  static PointerStatuses merge(const PointerStatuses& a, const PointerStatuses& b) {
    PointerStatuses merged;
    for (const auto& pair : a.map) {
      const Value* ptr = pair.getFirst();
      const PointerKind kind_in_a = pair.getSecond();
      const auto& it = b.map.find(ptr);
      PointerKind kind_in_b;
      if (it == b.map.end()) {
        // implicitly DIRTY in b
        kind_in_b = DIRTY;
      } else {
        kind_in_b = it->getSecond();
      }
      merged.mark_as(ptr, ::merge(kind_in_a, kind_in_b));
    }
    return merged;
  }

private:
  /// Pointers not appearing in this map are considered DIRTY.
  SmallDenseMap<const Value*, PointerKind, 8> map;
};

/// This holds the per-block state for the analysis
class PerBlockState {
public:
  PerBlockState() {}
  ~PerBlockState() {}

  /// The status of all pointers at the _beginning_ of the block.
  PointerStatuses ptrs_beg;
  /// The status of all pointers at the _end_ of the block.
  PointerStatuses ptrs_end;
};

class DLIMAnalysis {
public:
  /// Creates and initializes the Analysis but doesn't actually run the analysis.
  ///
  /// `trustLLVMStructTypes`: if `true`, then we will assume that, e.g., if we have
  /// a CLEAN pointer to a struct, and derive a pointer to the nth element of that
  /// struct, the resulting pointer is also CLEAN.
  /// This assumption could get us in trouble if the original "pointer to a struct"
  /// was actually a pointer to some smaller object, and was casted to this pointer type.
  /// E.g., this could happen if we incorrectly cast a `void*` to a struct pointer in C.
  DLIMAnalysis(Function &F, bool trustLLVMStructTypes) : F(F), trustLLVMStructTypes(trustLLVMStructTypes) {
    initialize_block_states();
  }
  ~DLIMAnalysis() {}

  typedef struct Counts {
    unsigned clean;
    unsigned blemished;
    unsigned dirty;
    unsigned unknown;
  } Counts;

  /// This struct holds the STATIC results of the analysis
  typedef struct Results {
    // How many loads have a clean/dirty pointer as address
    Counts load_addrs;
    // How many stores have a clean/dirty pointer as address (we don't count the
    // data being stored, even if it's a pointer)
    Counts store_addrs;
    // How many times are we storing a clean/dirty pointer to memory (this
    // doesn't care whether the address of the store is clean or dirty)
    Counts store_vals;
    // How many times are we passing a clean/dirty pointer to a function
    Counts passed_ptrs;
    // How many times are we returning a clean/dirty pointer from a function
    Counts returned_ptrs;
    // How many times did we produce a pointer via a 'inttoptr' instruction
    // (Note that we consider these to be dirty pointers)
    unsigned inttoptrs;
  } Results;

  /// Runs the analysis and returns the `Results` (static counts)
  Results run() {
    Results results;

    bool changed = true;
    while (changed) {
      LLVM_DEBUG(dbgs() << "DLIM: starting an iteration through function " << F.getName() << "\n");
      changed = this->doIteration(results);
    }

    return results;
  }

  void reportResults(Results& results) {
    dbgs() << "Static counts for " << F.getName() << ":\n";
    dbgs() << "Loads with clean addr: " << results.load_addrs.clean << "\n";
    dbgs() << "Loads with blemished addr: " << results.load_addrs.blemished << "\n";
    dbgs() << "Loads with dirty addr: " << results.load_addrs.dirty << "\n";
    dbgs() << "Loads with unknown addr: " << results.load_addrs.unknown << "\n";
    dbgs() << "Stores with clean addr: " << results.store_addrs.clean << "\n";
    dbgs() << "Stores with blemished addr: " << results.store_addrs.blemished << "\n";
    dbgs() << "Stores with dirty addr: " << results.store_addrs.dirty << "\n";
    dbgs() << "Stores with unknown addr: " << results.store_addrs.unknown << "\n";
    dbgs() << "Storing a clean ptr to mem: " << results.store_vals.clean << "\n";
    dbgs() << "Storing a blemished ptr to mem: " << results.store_vals.blemished << "\n";
    dbgs() << "Storing a dirty ptr to mem: " << results.store_vals.dirty << "\n";
    dbgs() << "Storing an unknown ptr to mem: " << results.store_vals.unknown << "\n";
    dbgs() << "Passing a clean ptr to a func: " << results.passed_ptrs.clean << "\n";
    dbgs() << "Passing a blemished ptr to a func: " << results.passed_ptrs.blemished << "\n";
    dbgs() << "Passing a dirty ptr to a func: " << results.passed_ptrs.dirty << "\n";
    dbgs() << "Passing an unknown ptr to a func: " << results.passed_ptrs.unknown << "\n";
    dbgs() << "Returning a clean ptr from a func: " << results.returned_ptrs.clean << "\n";
    dbgs() << "Returning a blemished ptr from a func: " << results.returned_ptrs.blemished << "\n";
    dbgs() << "Returning a dirty ptr from a func: " << results.returned_ptrs.dirty << "\n";
    dbgs() << "Returning an unknown ptr from a func: " << results.returned_ptrs.unknown << "\n";
    dbgs() << "Producing a ptr (assumed dirty) from inttoptr: " << results.inttoptrs << "\n";
    dbgs() << "\n";
  }

private:
  Function &F;

  bool trustLLVMStructTypes;

  DenseMap<const BasicBlock*, PerBlockState> block_states;

  void initialize_block_states() {
    for (const BasicBlock &block : F) {
      block_states.insert(
        std::pair<const BasicBlock*, PerBlockState>(&block, PerBlockState())
      );
    }

    // For now, if any function parameters are pointers,
    // mark them UNKNOWN in the function's entry block
    PerBlockState& entry_block_pbs = block_states.find(&F.getEntryBlock())->getSecond();
    for (const Argument& arg : F.args()) {
      if (arg.getType()->isPointerTy()) {
        entry_block_pbs.ptrs_beg.mark_unknown(&arg);
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
      if (block.hasName()) {
        LLVM_DEBUG(dbgs() << "DLIM: analyzing block " << block.getName() << " which previously had " << pbs.ptrs_beg.describe() << " at beginning and " << pbs.ptrs_end.describe() << " at end\n");
      } else {
        LLVM_DEBUG(dbgs() << "DLIM: analyzing block which previously had " << pbs.ptrs_beg.describe() << " at beginning and " << pbs.ptrs_end.describe() << " at end\n");
      }

      // first: if any variable is clean at the end of all of this block's
      // predecessors, then it is also clean at the beginning of this block
      if (!block.hasNPredecessors(0)) {
        auto preds = pred_begin(&block);
        const BasicBlock* firstPred = *preds;
        const PerBlockState& firstPred_pbs = block_states.find(firstPred)->getSecond();
        // we start with all of the ptr_statuses at the end of our first predecessor,
        // then merge with the ptr_statuses at the end of our other predecessors
        PointerStatuses ptr_statuses = PointerStatuses(firstPred_pbs.ptrs_end);
        LLVM_DEBUG(dbgs() << "DLIM:   first predecessor has " << ptr_statuses.describe() << " at end\n");
        for (auto it = ++preds, end = pred_end(&block); it != end; ++it) {
          const BasicBlock* otherPred = *it;
          const PerBlockState& otherPred_pbs = block_states.find(otherPred)->getSecond();
          LLVM_DEBUG(dbgs() << "DLIM:   next predecessor has " << otherPred_pbs.ptrs_end.describe() << " at end\n");
          ptr_statuses = PointerStatuses::merge(std::move(ptr_statuses), otherPred_pbs.ptrs_end);
        }
        // whatever's left is now the set of clean ptrs at beginning of this block
        changed |= !ptr_statuses.isEqualTo(pbs.ptrs_beg);
        pbs.ptrs_beg = std::move(ptr_statuses);
      }
      LLVM_DEBUG(dbgs() << "DLIM:   at beginning of block, we now have " << pbs.ptrs_beg.describe() << "\n");

      // The current pointer statuses. This begins as `pbs.ptrs_beg`, and as we
      // go through the block, gets updated; its state at the end of the block
      // will become `pbs.ptrs_end`.
      PointerStatuses ptr_statuses = PointerStatuses(pbs.ptrs_beg);

      // now: process each instruction
      // the only way for a dirty pointer to become clean is by being dereferenced
      // there is no way for a clean pointer to become dirty
      // so we only need to worry about pointer dereferences, and instructions
      // which produce pointers
      // (and of course we want to statically count clean/dirty loads/stores)
      for (const Instruction &inst : block) {
        switch (inst.getOpcode()) {
          case Instruction::Store: {
            const StoreInst& store = cast<StoreInst>(inst);
            // first count the stored value for static stats (if it's a pointer)
            const Value* storedVal = store.getValueOperand();
            if (storedVal->getType()->isPointerTy()) {
              const PointerKind kind = ptr_statuses.getStatus(storedVal);
              switch (kind) {
                case CLEAN: results.store_vals.clean++; break;
                case BLEMISHED: results.store_vals.blemished++; break;
                case DIRTY: results.store_vals.dirty++; break;
                case UNKNOWN: results.store_vals.unknown++; break;
                default: assert(false && "PointerKind case not handled");
              }
            }
            // next count the address for static stats
            const Value* addr = store.getPointerOperand();
            const PointerKind kind = ptr_statuses.getStatus(addr);
            switch (kind) {
              case CLEAN: results.store_addrs.clean++; break;
              case BLEMISHED: results.store_addrs.blemished++; break;
              case DIRTY: results.store_addrs.dirty++; break;
              case UNKNOWN: results.store_addrs.unknown++; break;
              default: assert(false && "PointerKind case not handled");
            }
            // now, the pointer used as an address becomes clean
            ptr_statuses.mark_clean(addr);
            break;
          }
          case Instruction::Load: {
            const LoadInst& load = cast<LoadInst>(inst);
            const Value* ptr = load.getPointerOperand();
            // first count this for static stats
            const PointerKind kind = ptr_statuses.getStatus(ptr);
            switch (kind) {
              case CLEAN: results.load_addrs.clean++; break;
              case BLEMISHED: results.load_addrs.blemished++; break;
              case DIRTY: results.load_addrs.dirty++; break;
              case UNKNOWN: results.load_addrs.unknown++; break;
              default: assert(false && "PointerKind case not handled");
            }
            // now, the pointer becomes clean
            ptr_statuses.mark_clean(ptr);

            if (load.getType()->isPointerTy()) {
              // in this case, we loaded a pointer from memory, and have to
              // worry about whether it's clean or not.
              // For now, we mark it UNKNOWN.
              ptr_statuses.mark_unknown(&load);
            }
            break;
          }
          case Instruction::Alloca: {
            // result of an alloca is a clean pointer
            ptr_statuses.mark_clean(&inst);
            break;
          }
          case Instruction::GetElementPtr: {
            const GetElementPtrInst& gep = cast<GetElementPtrInst>(inst);
            const Value* input_ptr = gep.getPointerOperand();
            PointerKind input_kind = ptr_statuses.getStatus(input_ptr);
            APInt offset = APInt(/* bits = */ 64, /* val = */ 0);
            const DataLayout& DL = F.getParent()->getDataLayout();
            const bool offsetIsConstant = gep.accumulateConstantOffset(DL, offset);  // `offset` is only valid if `offsetIsConstant`
            if (gep.hasAllZeroIndices()) {
              // result of a GEP with all zeroes as indices, is the same as the input pointer.
              assert(offsetIsConstant && offset == APInt(/* bits = */ 64, /* val = */ 0) && "If all indices are constant 0, then the total offset should be constant 0");
              ptr_statuses.mark_as(&gep, input_kind);
            } else if (trustLLVMStructTypes && areAllIndicesTrustworthy(gep)) {
              // nonzero offset, but "trustworthy" offset.
              // here output pointer is the same kind as input pointer.
              // but as an exception, if input pointer was blemished, this is
              // still an increment, so output pointer is dirty.
              if (input_kind == BLEMISHED) {
                ptr_statuses.mark_dirty(&gep);
              } else {
                ptr_statuses.mark_as(&gep, input_kind);
              }
            } else if (input_kind == CLEAN) {
              // result of a GEP with any nonzero indices, on a CLEAN pointer,
              // is either DIRTY or BLEMISHED depending on how far the pointer
              // arithmetic goes.
              if (offsetIsConstant && offset.ule(APInt(/* bits = */ 64, /* val = */ BLEMISHED_BYTES))) {
                ptr_statuses.mark_blemished(&gep);
              } else {
                // offset is too large or is not constant; so, result is dirty
                ptr_statuses.mark_dirty(&gep);
              }
            } else {
              // result of a GEP with any nonzero indices, on a DIRTY,
              // BLEMISHED, or UNKNOWN pointer, is always DIRTY.
              ptr_statuses.mark_dirty(&gep);
            }
            break;
          }
          case Instruction::BitCast: {
            const BitCastInst& bitcast = cast<BitCastInst>(inst);
            if (bitcast.getType()->isPointerTy()) {
              const Value* input_ptr = bitcast.getOperand(0);
              ptr_statuses.mark_as(&bitcast, ptr_statuses.getStatus(input_ptr));
            }
            break;
          }
          case Instruction::Select: {
            const SelectInst& select = cast<SelectInst>(inst);
            if (select.getType()->isPointerTy()) {
              // output is clean if both inputs are clean; etc
              const Value* true_input = select.getTrueValue();
              const Value* false_input = select.getFalseValue();
              const PointerKind true_kind = ptr_statuses.getStatus(true_input);
              const PointerKind false_kind = ptr_statuses.getStatus(false_input);
              ptr_statuses.mark_as(&select, merge(true_kind, false_kind));
            }
            break;
          }
          case Instruction::PHI: {
            const PHINode& phi = cast<PHINode>(inst);
            if (phi.getType()->isPointerTy()) {
              // phi: result kind is the merger of the kinds of all the inputs
              // in their corresponding blocks
              PointerKind merged_kind = CLEAN;
              for (const Use& use : phi.incoming_values()) {
                const BasicBlock* bb = phi.getIncomingBlock(use);
                auto& ptr_statuses_end_of_bb = block_states.find(bb)->getSecond().ptrs_end;
                const Value* value = use.get();
                merged_kind = merge(merged_kind, ptr_statuses_end_of_bb.getStatus(value));
              }
              ptr_statuses.mark_as(&phi, merged_kind);
            }
            break;
          }
          case Instruction::IntToPtr: {
            // inttoptr always produces a dirty result
            results.inttoptrs++;
            ptr_statuses.mark_dirty(&inst);
            break;
          }
          case Instruction::Call: {
            const CallInst& call = cast<CallInst>(inst);
            // count call arguments for static stats
            for (const Use& arg : call.args()) {
              const Value* value = arg.get();
              if (value->getType()->isPointerTy()) {
                const PointerKind kind = ptr_statuses.getStatus(value);
                switch (kind) {
                  case CLEAN: results.passed_ptrs.clean++; break;
                  case BLEMISHED: results.passed_ptrs.blemished++; break;
                  case DIRTY: results.passed_ptrs.dirty++; break;
                  case UNKNOWN: results.passed_ptrs.unknown++; break;
                  default: assert(false && "PointerKind case not handled");
                }
              }
            }
            // For now, mark pointers returned from calls as UNKNOWN
            ptr_statuses.mark_unknown(&call);
            break;
          }
          case Instruction::ExtractValue: {
            // this gets a pointer out of a field of a first-class struct (not a
            // pointer-to-a-struct, so ptr_statuses doesn't have any information
            // about this struct).
            // the question is where did the struct come from.
            // As I see it, probably either (a) we created the struct with
            // insertvalue - in which case, ideally we'd give this result
            // the same PointerKind as the original pointer which was inserted;
            // or (b) we loaded the struct from memory (?), in which case we
            // should just mark the result UNKNOWN per our current assumptions.
            // So for now, we'll just mark UNKNOWN and move on
            ptr_statuses.mark_unknown(&inst);
            break;
          }
          case Instruction::Ret: {
            const ReturnInst& ret = cast<ReturnInst>(inst);
            const Value* retval = ret.getReturnValue();
            if (retval && retval->getType()->isPointerTy()) {
              const PointerKind kind = ptr_statuses.getStatus(retval);
              switch (kind) {
                case CLEAN: results.returned_ptrs.clean++; break;
                case BLEMISHED: results.returned_ptrs.blemished++; break;
                case DIRTY: results.returned_ptrs.dirty++; break;
                case UNKNOWN: results.returned_ptrs.unknown++; break;
                default: assert(false && "PointerKind case not handled");
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

      // Now that we've processed all the instructions, we have the final
      // statuses of pointers as of the end of the block
      LLVM_DEBUG(dbgs() << "DLIM:   at end of block, we now have " << ptr_statuses.describe() << "\n");
      const bool block_changed = !ptr_statuses.isEqualTo(pbs.ptrs_end);
      if (block_changed) {
        LLVM_DEBUG(dbgs() << "DLIM:   this was a change\n");
      }
      changed |= block_changed;
      pbs.ptrs_end = std::move(ptr_statuses);
    }

    return changed;
  }
};

PreservedAnalyses StaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, true);
  DLIMAnalysis::Results results = analysis.run();
  analysis.reportResults(results);

  // StaticDLIMPass only analyzes the IR and doesn't make any changes, so all
  // analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses ParanoidStaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, false);
  DLIMAnalysis::Results results = analysis.run();
  analysis.reportResults(results);

  // ParanoidStaticDLIMPass only analyzes the IR and doesn't make any changes,
  // so all analyses are preserved
  return PreservedAnalyses::all();
}

static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep) {
  //LLVM_DEBUG(dbgs() << "Analyzing the following gep:\n");
  //LLVM_DEBUG(gep.dump());
  Type* current_ty = gep.getPointerOperandType();
  SmallVector<Constant*, 8> seen_indices;
  for (const Use& idx : gep.indices()) {
    if (!current_ty) {
      LLVM_DEBUG(dbgs() << "current_ty is null - probably getIndexedType() returned null\n");
      return false;
    }
    if (ConstantInt* c = dyn_cast<ConstantInt>(idx.get())) {
      //LLVM_DEBUG(dbgs() << "Encountered constant index " << c->getSExtValue() << "\n");
      //LLVM_DEBUG(dbgs() << "Current ty is " << *current_ty << "\n");
      seen_indices.push_back(cast<Constant>(c));
      if (c->isZero()) {
        // zero is always trustworthy
        //LLVM_DEBUG(dbgs() << "zero is always trustworthy\n");
      } else {
        // constant, nonzero index
        if (seen_indices.size() == 1) {
          // the first time is just selecting the element of the implied array.
          //LLVM_DEBUG(dbgs() << "indexing into an implicit array is not trustworthy\n");
          return false;
        }
        const PointerType* current_ty_as_ptrtype = cast<const PointerType>(current_ty);
        const Type* current_pointee_ty = current_ty_as_ptrtype->getElementType();
        //LLVM_DEBUG(dbgs() << "Current pointee ty is " << *current_pointee_ty << "\n");
        if (current_pointee_ty->isStructTy()) {
          // trustworthy
          //LLVM_DEBUG(dbgs() << "indexing into a struct ty is trustworthy\n");
        } else if (current_pointee_ty->isArrayTy()) {
          // not trustworthy
          //LLVM_DEBUG(dbgs() << "indexing into an array ty is not trustworthy\n");
          return false;
        } else {
          // implicit array type. e.g., indexing into an i32*.
          //LLVM_DEBUG(dbgs() << "indexing into an implicit array is not trustworthy\n");
          return false;
        }
      }
    } else {
      // any nonconstant index? then return false
      return false;
    }
    if (seen_indices.size() == 1) {
      // the first time, we don't update `current_ty`, because the first GEP index
      // is just selecting the element of the implied array
    } else {
      ArrayRef<Constant*> seen_indices_arrayref = ArrayRef<Constant*>(seen_indices);
      current_ty = GetElementPtrInst::getIndexedType(gep.getPointerOperandType(), seen_indices_arrayref);
    }
  }
  // if we get here without finding a non-trustworthy index, then we're all good
  return true;
}
