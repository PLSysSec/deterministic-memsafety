#include "llvm/Transforms/Utils/DLIM.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Regex.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

#include <sstream>  // ostringstream

using namespace llvm;

#define DEBUG_TYPE "DLIM"

static bool areAllIndicesTrustworthy(const GetElementPtrInst &gep);
static Constant* createGlobalConstStr(Module* mod, const char* global_name, const char* str);
static std::string regexSubAll(const Regex &R, const StringRef Repl, const StringRef String);

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

  typedef struct StaticCounts {
    unsigned clean;
    unsigned blemished;
    unsigned dirty;
    unsigned unknown;
  } StaticCounts;

  /// This struct holds the STATIC results of the analysis
  typedef struct StaticResults {
    // How many loads have a clean/dirty pointer as address
    StaticCounts load_addrs;
    // How many stores have a clean/dirty pointer as address (we don't count the
    // data being stored, even if it's a pointer)
    StaticCounts store_addrs;
    // How many times are we storing a clean/dirty pointer to memory (this
    // doesn't care whether the address of the store is clean or dirty)
    StaticCounts store_vals;
    // How many times are we passing a clean/dirty pointer to a function
    StaticCounts passed_ptrs;
    // How many times are we returning a clean/dirty pointer from a function
    StaticCounts returned_ptrs;
    // How many times did we produce a pointer via a 'inttoptr' instruction
    // (Note that we consider these to be dirty pointers)
    unsigned inttoptrs;
  } StaticResults;

  /// Holds the IR global variables containing dynamic counts
  typedef struct DynamicCounts {
    Constant* clean;
    Constant* blemished;
    Constant* dirty;
    Constant* unknown;
  } DynamicCounts;

  /// This struct holds the IR global variables representing the DYNAMIC results
  /// of the analysis
  typedef struct DynamicResults {
    // How many loads have a clean/dirty pointer as address
    DynamicCounts load_addrs;
    // How many stores have a clean/dirty pointer as address (we don't count the
    // data being stored, even if it's a pointer)
    DynamicCounts store_addrs;
    // How many times are we storing a clean/dirty pointer to memory (this
    // doesn't care whether the address of the store is clean or dirty)
    DynamicCounts store_vals;
    // How many times are we passing a clean/dirty pointer to a function
    DynamicCounts passed_ptrs;
    // How many times are we returning a clean/dirty pointer from a function
    DynamicCounts returned_ptrs;
    // How many times did we produce a pointer via a 'inttoptr' instruction
    // (Note that we consider these to be dirty pointers)
    Constant* inttoptrs;
  } DynamicResults;

  /// Runs the analysis and returns the `StaticResults`
  StaticResults run() {
    StaticResults static_results;

    bool changed = true;
    while (changed) {
      LLVM_DEBUG(dbgs() << "DLIM: starting an iteration through function " << F.getName() << "\n");
      changed = doIteration(static_results, NULL, false);
    }

    return static_results;
  }

  /// Where to print results dynamically (at runtime)
  typedef enum DynamicPrintType {
    /// Print to stdout
    STDOUT,
    /// Print to a file in ./dlim_dynamic_counts
    TOFILE,
  } DynamicPrintType;

  /// Instruments the code for dynamic counts.
  /// You _must_ run() the analysis first -- instrument() assumes that the
  /// analysis is complete.
  void instrument(DynamicPrintType print_type) {
    DynamicResults results = initializeDynamicResults();
    StaticResults _ignore;
    bool changed = doIteration(_ignore, &results, true);
    assert(!changed && "Don't run instrument() until analysis has reached fixpoint");
    addDynamicResultsPrint(results, print_type);
  }

  void reportStaticResults(StaticResults& results) {
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
    PerBlockState& entry_block_pbs = block_states[&F.getEntryBlock()];
    for (const Argument& arg : F.args()) {
      if (arg.getType()->isPointerTy()) {
        entry_block_pbs.ptrs_beg.mark_unknown(&arg);
      }
    }

    // Mark pointers to global variables as CLEAN in the function's entry block
    // (if the global variable itself is a pointer, it's still implicitly dirty)
    for (const GlobalVariable& gv : F.getParent()->globals()) {
      assert(gv.getType()->isPointerTy());
      entry_block_pbs.ptrs_beg.mark_clean(&gv);
    }
  }

  /// `instrument`: if `true`, insert instrumentation to collect dynamic counts.
  /// Do this only after the analysis has reached a fixpoint.
  ///
  /// Returns `true` if any change was made to internal state (not counting the
  /// results objects of course)
  bool doIteration(StaticResults &static_results, DynamicResults* dynamic_results, bool instrument) {
    // Reset the static results - we'll collect them new
    static_results = { 0 };

    bool changed = false;

    for (BasicBlock &block : F) {
      PerBlockState& pbs = block_states[&block];
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
        const PerBlockState& firstPred_pbs = block_states[firstPred];
        // we start with all of the ptr_statuses at the end of our first predecessor,
        // then merge with the ptr_statuses at the end of our other predecessors
        PointerStatuses ptr_statuses = PointerStatuses(firstPred_pbs.ptrs_end);
        LLVM_DEBUG(dbgs() << "DLIM:   first predecessor has " << ptr_statuses.describe() << " at end\n");
        for (auto it = ++preds, end = pred_end(&block); it != end; ++it) {
          const BasicBlock* otherPred = *it;
          const PerBlockState& otherPred_pbs = block_states[otherPred];
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
      for (Instruction &inst : block) {
        switch (inst.getOpcode()) {
          case Instruction::Store: {
            const StoreInst& store = cast<StoreInst>(inst);
            // first count the stored value (if it's a pointer)
            const Value* storedVal = store.getValueOperand();
            if (storedVal->getType()->isPointerTy()) {
              const PointerKind kind = ptr_statuses.getStatus(storedVal);
              switch (kind) {
                case CLEAN: {
                  static_results.store_vals.clean++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_vals.clean, &inst);
                  }
                  break;
                }
                case BLEMISHED: {
                  static_results.store_vals.blemished++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_vals.blemished, &inst);
                  }
                  break;
                }
                case DIRTY: {
                  static_results.store_vals.dirty++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_vals.dirty, &inst);
                  }
                  break;
                }
                case UNKNOWN: {
                  static_results.store_vals.unknown++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_vals.unknown, &inst);
                  }
                  break;
                }
                default: assert(false && "PointerKind case not handled");
              }
            }
            // next count the address
            const Value* addr = store.getPointerOperand();
            const PointerKind kind = ptr_statuses.getStatus(addr);
            switch (kind) {
              case CLEAN: {
                  static_results.store_addrs.clean++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_addrs.clean, &inst);
                  }
                  break;
                }
              case BLEMISHED: {
                  static_results.store_addrs.blemished++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_addrs.blemished, &inst);
                  }
                  break;
                }
              case DIRTY: {
                  static_results.store_addrs.dirty++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_addrs.dirty, &inst);
                  }
                  break;
                }
              case UNKNOWN: {
                  static_results.store_addrs.unknown++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->store_addrs.unknown, &inst);
                  }
                  break;
                }
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
              case CLEAN: {
                  static_results.load_addrs.clean++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->load_addrs.clean, &inst);
                  }
                  break;
                }
              case BLEMISHED: {
                  static_results.load_addrs.blemished++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->load_addrs.blemished, &inst);
                  }
                  break;
                }
              case DIRTY: {
                  static_results.load_addrs.dirty++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->load_addrs.dirty, &inst);
                  }
                  break;
                }
              case UNKNOWN: {
                  static_results.load_addrs.unknown++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->load_addrs.unknown, &inst);
                  }
                  break;
                }
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
                auto& ptr_statuses_end_of_bb = block_states[bb].ptrs_end;
                const Value* value = use.get();
                merged_kind = merge(merged_kind, ptr_statuses_end_of_bb.getStatus(value));
              }
              ptr_statuses.mark_as(&phi, merged_kind);
            }
            break;
          }
          case Instruction::IntToPtr: {
            // inttoptr always produces a dirty result
            static_results.inttoptrs++;
            if (instrument) {
              incrementGlobalCounter(dynamic_results->inttoptrs, &inst);
            }
            ptr_statuses.mark_dirty(&inst);
            break;
          }
          case Instruction::Call:
          case Instruction::CallBr:
          case Instruction::Invoke:
          // all three of these are instructions which call functions, and we
          // handle them the same
          {
            const CallBase& call = cast<CallBase>(inst);
            // count call arguments for static stats
            for (const Use& arg : call.args()) {
              const Value* value = arg.get();
              if (value->getType()->isPointerTy()) {
                const PointerKind kind = ptr_statuses.getStatus(value);
                switch (kind) {
                  case CLEAN: {
                    static_results.passed_ptrs.clean++;
                    if (instrument) {
                      incrementGlobalCounter(dynamic_results->passed_ptrs.clean, &inst);
                    }
                    break;
                  }
                  case BLEMISHED: {
                    static_results.passed_ptrs.blemished++;
                    if (instrument) {
                      incrementGlobalCounter(dynamic_results->passed_ptrs.blemished, &inst);
                    }
                    break;
                  }
                  case DIRTY: {
                    static_results.passed_ptrs.dirty++;
                    if (instrument) {
                      incrementGlobalCounter(dynamic_results->passed_ptrs.dirty, &inst);
                    }
                    break;
                  }
                  case UNKNOWN: {
                    static_results.passed_ptrs.unknown++;
                    if (instrument) {
                      incrementGlobalCounter(dynamic_results->passed_ptrs.unknown, &inst);
                    }
                    break;
                  }
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
          case Instruction::ExtractElement: {
            // same comments apply as for ExtractValue, basically
            ptr_statuses.mark_unknown(&inst);
            break;
          }
          case Instruction::Ret: {
            const ReturnInst& ret = cast<ReturnInst>(inst);
            const Value* retval = ret.getReturnValue();
            if (retval && retval->getType()->isPointerTy()) {
              const PointerKind kind = ptr_statuses.getStatus(retval);
              switch (kind) {
                case CLEAN: {
                  static_results.returned_ptrs.clean++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->returned_ptrs.clean, &inst);
                  }
                  break;
                }
                case BLEMISHED: {
                  static_results.returned_ptrs.blemished++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->returned_ptrs.blemished, &inst);
                  }
                  break;
                }
                case DIRTY: {
                  static_results.returned_ptrs.dirty++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->returned_ptrs.dirty, &inst);
                  }
                  break;
                }
                case UNKNOWN: {
                  static_results.returned_ptrs.unknown++;
                  if (instrument) {
                    incrementGlobalCounter(dynamic_results->returned_ptrs.unknown, &inst);
                  }
                  break;
                }
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

  DynamicResults initializeDynamicResults() {
    DynamicCounts load_addrs = initializeDynamicCounts("__DLIM_load_addrs");
    DynamicCounts store_addrs = initializeDynamicCounts("__DLIM_store_addrs");
    DynamicCounts store_vals = initializeDynamicCounts("__DLIM_store_vals");
    DynamicCounts passed_ptrs = initializeDynamicCounts("__DLIM_passed_ptrs");
    DynamicCounts returned_ptrs = initializeDynamicCounts("__DLIM_returned_ptrs");
    Constant* inttoptrs = findOrCreateGlobalCounter("__DLIM_inttoptrs");
    return DynamicResults { load_addrs, store_addrs, store_vals, passed_ptrs, returned_ptrs, inttoptrs };
  }

  DynamicCounts initializeDynamicCounts(StringRef thingToCount) {
    Constant* clean = findOrCreateGlobalCounter(thingToCount + "_clean");
    Constant* blemished = findOrCreateGlobalCounter(thingToCount + "_blemished");
    Constant* dirty = findOrCreateGlobalCounter(thingToCount + "_dirty");
    Constant* unknown = findOrCreateGlobalCounter(thingToCount + "_unknown");
    return DynamicCounts { clean, blemished, dirty, unknown };
  }

  Constant* findOrCreateGlobalCounter(Twine Name) {
    // https://github.com/banach-space/llvm-tutor/blob/0d2864d19b90fbcc31cea530ec00215405271e40/lib/DynamicCallCounter.cpp
    Module* mod = F.getParent();
    LLVMContext& ctx = mod->getContext();
    Type* i64ty = IntegerType::getInt64Ty(ctx);

    Constant* global = mod->getOrInsertGlobal(Name.str(), i64ty);
    GlobalVariable* gv = cast<GlobalVariable>(global);
    if (!gv->hasInitializer()) {
      gv->setLinkage(GlobalValue::PrivateLinkage);
      gv->setAlignment(MaybeAlign(8));
      gv->setInitializer(ConstantInt::get(ctx, APInt(/* bits = */ 64, /* val = */ 0)));
    }

    return global;
  }

  // Inject an instruction sequence to increment the given global counter, right
  // before the given instruction
  void incrementGlobalCounter(Constant* GlobalCounter, Instruction* BeforeInst) {
    IRBuilder<> Builder(BeforeInst);
    Type* i64ty = Builder.getInt64Ty();
    LoadInst* loaded = Builder.CreateLoad(i64ty, GlobalCounter);
    Value* incremented = Builder.CreateAdd(Builder.getInt64(1), loaded);
    Builder.CreateStore(incremented, GlobalCounter);
  }

  void addDynamicResultsPrint(DynamicResults& dynamic_results, DynamicPrintType print_type) {
    // https://github.com/banach-space/llvm-tutor/blob/0d2864d19b90fbcc31cea530ec00215405271e40/lib/DynamicCallCounter.cpp
    Module* mod = F.getParent();
    LLVMContext& ctx = mod->getContext();

    // if this function already exists in the module, assume we've already added
    // the print
    if (mod->getFunction("__DLIM_output_wrapper")) {
      return;
    }

    std::string output = "";
    output += "================\n";
    output += "DLIM dynamic counts for " + mod->getName().str() + ":\n";
    output += "================\n";
    output += "Loads with clean addr: %llu\n";
    output += "Loads with blemished addr: %llu\n";
    output += "Loads with dirty addr: %llu\n";
    output += "Loads with unknown addr: %llu\n";
    output += "Stores with clean addr: %llu\n";
    output += "Stores with blemished addr: %llu\n";
    output += "Stores with dirty addr: %llu\n";
    output += "Stores with unknown addr: %llu\n";
    output += "Storing a clean ptr to mem: %llu\n";
    output += "Storing a blemished ptr to mem: %llu\n";
    output += "Storing a dirty ptr to mem: %llu\n";
    output += "Storing an unknown ptr to mem: %llu\n";
    output += "Passing a clean ptr to a func: %llu\n";
    output += "Passing a blemished ptr to a func: %llu\n";
    output += "Passing a dirty ptr to a func: %llu\n";
    output += "Passing an unknown ptr to a func: %llu\n";
    output += "Returning a clean ptr from a func: %llu\n";
    output += "Returning a blemished ptr from a func: %llu\n";
    output += "Returning a dirty ptr from a func: %llu\n";
    output += "Returning an unknown ptr from a func: %llu\n";
    output += "Producing a ptr (assumed dirty) from inttoptr: %llu\n";
    output += "\n";

    // Inject a global variable to hold the output string
    Constant* OutputStr = createGlobalConstStr(mod, "__DLIM_output_str", output.c_str());

    // Create a void function which calls printf() or fprintf() to print the
    // output
    Type* i8ty = IntegerType::getInt8Ty(ctx);
    Type* i8StarTy = PointerType::getUnqual(i8ty);
    Type* i32ty = IntegerType::getInt32Ty(ctx);
    Type* i32StarTy = PointerType::getUnqual(i32ty);
    Type* i64ty = IntegerType::getInt64Ty(ctx);
    FunctionType* WrapperTy = FunctionType::get(Type::getVoidTy(ctx), {}, false);
    Function* Wrapper_func = cast<Function>(mod->getOrInsertFunction("__DLIM_output_wrapper", WrapperTy).getCallee());
    Wrapper_func->setLinkage(GlobalValue::PrivateLinkage);
    BasicBlock* EntryBlock = BasicBlock::Create(ctx, "entry", Wrapper_func);
    IRBuilder<> Builder(EntryBlock);

    if (print_type == STDOUT) {
      // call printf()
      FunctionType* PrintfTy = FunctionType::get(i32ty, i8StarTy, /* IsVarArgs = */ true);
      FunctionCallee Printf = mod->getOrInsertFunction("printf", PrintfTy);
      //Function* Printf_func = cast<Function>(Printf.getCallee());
      //Printf_func->setDoesNotThrow();
      //Printf_func->addParamAttr(0, Attribute::NoCapture);
      //Printf_func->addParamAttr(0, Attribute::ReadOnly);
      Builder.CreateCall(Printf, {
        Builder.CreatePointerCast(OutputStr, i8StarTy),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.inttoptrs),
      });
      Builder.CreateRetVoid();
    } else if (print_type == TOFILE) {
      // create strings for arguments to mkdir, fopen, and perror
      auto modNameNoDotDot = regexSubAll(Regex("\\.\\./"), "", mod->getName());
      auto modNameWithDots = regexSubAll(Regex("/"), ".", modNameNoDotDot);
      auto file_str = "./dlim_dynamic_counts/" + modNameWithDots;
      Constant* DirStr = createGlobalConstStr(mod, "__DLIM_dir_str", "./dlim_dynamic_counts");
      Constant* FileStr = createGlobalConstStr(mod, "__DLIM_file_str", file_str.c_str());
      Constant* ModeStr = createGlobalConstStr(mod, "__DLIM_mode_str", "a");
      Constant* PerrorStr = createGlobalConstStr(mod, "__DLIM_perror_str", ("Failed to open " + file_str).c_str());
      // call mkdir
      FunctionType* MkdirTy = FunctionType::get(i32ty, {i8StarTy, i32ty}, /* IsVarArgs = */ false);
      FunctionCallee Mkdir = mod->getOrInsertFunction("mkdir", MkdirTy);
      //Function* Mkdir_func = cast<Function>(Mkdir.getCallee());
      //Mkdir_func->addParamAttr(0, Attribute::NoCapture);
      //Mkdir_func->addParamAttr(0, Attribute::ReadOnly);
      Value* Mkdir_ret = Builder.CreateCall(Mkdir, {
        Builder.CreatePointerCast(DirStr, i8StarTy),
        Builder.getInt32(/* octal */ 0777),
      });
      // check for error from mkdir
      Value* cond = Builder.CreateICmpEQ(Mkdir_ret, Builder.getInt32(0));
      BasicBlock* ErrorBB = BasicBlock::Create(ctx, "error", Wrapper_func);
      BasicBlock* NoErrorBB = BasicBlock::Create(ctx, "noerror", Wrapper_func);
      Builder.CreateCondBr(cond, NoErrorBB, ErrorBB);
      // the case where mkdir returns error
      // see if errno is EEXIST (17), and if so, ignore the error.
      // otherwise return early and don't print anything.
      Builder.SetInsertPoint(ErrorBB);
      FunctionType* ErrnoTy = FunctionType::get(i32StarTy, {}, false);
      FunctionCallee Errno_callee = mod->getOrInsertFunction("__errno_location", ErrnoTy);
      //Function* Errno_func = cast<Function>(Errno_callee.getCallee());
      //Errno_func->setDoesNotAccessMemory();
      //Errno_func->setDoesNotThrow();
      //Errno_func->setWillReturn();
      Value* errno_addr = Builder.CreateCall(Errno_callee, {});
      Value* errno_val = Builder.CreateLoad(i32ty, errno_addr);
      cond = Builder.CreateICmpEQ(errno_val, Builder.getInt32(17));
      BasicBlock* JustReturnBB = BasicBlock::Create(ctx, "justreturn", Wrapper_func);
      Builder.CreateCondBr(cond, NoErrorBB, JustReturnBB);
      Builder.SetInsertPoint(JustReturnBB);
      Builder.CreateRetVoid();
      // the case where mkdir succeeds (or where we got EEXIST and ignored it -
      // in either case the directory now exists).
      // Call fopen
      Builder.SetInsertPoint(NoErrorBB);
      StructType* FileTy = StructType::create(ctx, "struct._IO_FILE");
      PointerType* FileStarTy = PointerType::getUnqual(FileTy);
      FunctionType* FopenTy = FunctionType::get(FileStarTy, {i8StarTy, i8StarTy}, false);
      FunctionCallee Fopen = mod->getOrInsertFunction("fopen", FopenTy);
      //Function* Fopen_func = cast<Function>(Fopen.getCallee());
      //Fopen_func->addParamAttr(0, Attribute::NoCapture);
      //Fopen_func->addParamAttr(0, Attribute::ReadOnly);
      //Fopen_func->addParamAttr(1, Attribute::NoCapture);
      //Fopen_func->addParamAttr(1, Attribute::ReadOnly);
      Value* file_handle = Builder.CreateCall(Fopen, {
        Builder.CreatePointerCast(FileStr, i8StarTy),
        Builder.CreatePointerCast(ModeStr, i8StarTy),
      });
      // check for error from fopen
      cond = Builder.CreateIsNull(file_handle);
      BasicBlock* WriteBB = BasicBlock::Create(ctx, "write", Wrapper_func);
      BasicBlock* FopenFailed = BasicBlock::Create(ctx, "fopenfailed", Wrapper_func);
      Builder.CreateCondBr(cond, FopenFailed, WriteBB);
      Builder.SetInsertPoint(FopenFailed);
      FunctionType* PerrorTy = FunctionType::get(Type::getVoidTy(ctx), {i8StarTy}, false);
      FunctionCallee Perror = mod->getOrInsertFunction("perror", PerrorTy);
      Builder.CreateCall(Perror, {Builder.CreatePointerCast(PerrorStr, i8StarTy)});
      Builder.CreateRetVoid();
      // and, actually write to file
      Builder.SetInsertPoint(WriteBB);
      FunctionType* FprintfTy = FunctionType::get(i32ty, {FileStarTy, i8StarTy}, /* IsVarArgs = */ true);
      FunctionCallee Fprintf = mod->getOrInsertFunction("fprintf", FprintfTy);
      Builder.CreateCall(Fprintf, {
        file_handle,
        Builder.CreatePointerCast(OutputStr, i8StarTy),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.load_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_addrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.clean),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.store_vals.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.passed_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.clean),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.blemished),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.dirty),
        Builder.CreateLoad(i64ty, dynamic_results.returned_ptrs.unknown),
        Builder.CreateLoad(i64ty, dynamic_results.inttoptrs),
      });
      FunctionType* FcloseTy = FunctionType::get(i32ty, {FileStarTy}, false);
      FunctionCallee Fclose = mod->getOrInsertFunction("fclose", FcloseTy);
      Builder.CreateCall(Fclose, {file_handle});
      Builder.CreateBr(JustReturnBB);
    } else {
      assert(false && "unexpected print_type\n");
    }

    // Inject this wrapper function into the GlobalDtors for the module
    appendToGlobalDtors(*mod, Wrapper_func, /* Priority = */ 0);
  }
};

PreservedAnalyses StaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, true);
  DLIMAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // StaticDLIMPass only analyzes the IR and doesn't make any changes, so all
  // analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses ParanoidStaticDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  DLIMAnalysis analysis = DLIMAnalysis(F, false);
  DLIMAnalysis::StaticResults static_results = analysis.run();
  analysis.reportStaticResults(static_results);

  // ParanoidStaticDLIMPass only analyzes the IR and doesn't make any changes,
  // so all analyses are preserved
  return PreservedAnalyses::all();
}

PreservedAnalyses DynamicDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // Don't do any analysis or instrumentation on the special function __DLIM_output_wrapper
  if (F.getName() == "__DLIM_output_wrapper") {
    return PreservedAnalyses::all();
  }

  DLIMAnalysis analysis = DLIMAnalysis(F, true);
  analysis.run();
  analysis.instrument(DLIMAnalysis::DynamicPrintType::TOFILE);

  // For now we conservatively just tell LLVM that no analyses are preserved.
  // It seems that many existing LLVM passes also just use
  // PreservedAnalyses::none() when they make any change, so we assume this is
  // reasonable.
  return PreservedAnalyses::none();
}

PreservedAnalyses DynamicStdoutDLIMPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // Don't do any analysis or instrumentation on the special function __DLIM_output_wrapper
  if (F.getName() == "__DLIM_output_wrapper") {
    return PreservedAnalyses::all();
  }

  DLIMAnalysis analysis = DLIMAnalysis(F, true);
  analysis.run();
  analysis.instrument(DLIMAnalysis::DynamicPrintType::STDOUT);

  // For now we conservatively just tell LLVM that no analyses are preserved.
  // It seems that many existing LLVM passes also just use
  // PreservedAnalyses::none() when they make any change, so we assume this is
  // reasonable.
  return PreservedAnalyses::none();
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

static Constant* createGlobalConstStr(Module* mod, const char* global_name, const char* str) {
  LLVMContext& ctx = mod->getContext();
  Constant* strConst = ConstantDataArray::getString(ctx, str);
  Constant* strGlobal = mod->getOrInsertGlobal(global_name, strConst->getType());
  cast<GlobalVariable>(strGlobal)->setInitializer(strConst);
  cast<GlobalVariable>(strGlobal)->setLinkage(GlobalValue::PrivateLinkage);
  return strGlobal;
}

static std::string regexSubAll(const Regex &R, const StringRef Repl, const StringRef String) {
  std::string curString = String.str();
  while (R.match(curString)) {
    curString = R.sub(Repl, curString);
  }
  return curString;
}
