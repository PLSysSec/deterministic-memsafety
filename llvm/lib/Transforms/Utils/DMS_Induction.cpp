#include "llvm/Transforms/Utils/DMS_Induction.h"
#include <optional>

using namespace llvm;

#define DEBUG_TYPE "DMS-loop-induction"

static const APInt zero = APInt(/* bits = */ 64, /* val = */ 0);
static const InductionPatternResult no_induction_pattern = { false, zero, zero };

/// Return type for `isInductionVar`.
///
/// If the given `val` is an induction variable, then `val` is equal to
/// `initial_val` on the first loop iteration, and is incremented by
/// `induction_increment` each iteration.
struct InductionVar {
  bool is_induction_var;
  APInt induction_increment;
  APInt initial_val;
};
/// Is the given `val` an induction variable?
/// Here, "induction variable" is narrowly defined as:
///     a PHI between a constant (initial value) and a variable (induction)
///     equal to itself plus or minus a constant
///
/// If so, return the `InductionVar`; otherwise, return nothing
static std::optional<InductionVar> isInductionVar(const Value* val);

/// Return type for `isValuePlusConstant`. The given value is equal to `value`
/// plus `constant`.
struct ValPlusConstant {
  const Value* value;
  APInt constant;
};
/// Is the given `val` defined as some other `Value` plus/minus a constant?
///
/// If so, return the `ValPlusConstant`; otherwise, return nothing
static std::optional<ValPlusConstant> isValuePlusConstant(const Value* val);

/// Is the offset of the given GEP an induction pattern?
/// This is looking for a pretty specific pattern for GEPs inside loops, which
/// we can optimize checks for.
InductionPatternResult llvm::isOffsetAnInductionPattern(
  const GetElementPtrInst &gep,
  const DataLayout &DL,
  const LoopInfo& loopinfo,
  const PostDominatorTree& pdtree
) {
  LLVM_DEBUG(dbgs() << "DMS:   Checking the following gep for induction:\n");
  LLVM_DEBUG(gep.dump());
  if (gep.getNumIndices() != 1) return no_induction_pattern; // we only handle simple cases for now
  for (const Use& idx_as_use : gep.indices()) {
    // note that this for loop goes exactly one iteration, due to the check above.
    // `idx` will be the one index of the GEP.
    const Value* idx = idx_as_use.get();
    std::optional<InductionVar> iv = isInductionVar(idx);
    if (iv.has_value()) {
      LLVM_DEBUG(dbgs() << "DMS:     GEP single index is an induction var\n");
    } else {
      const std::optional<ValPlusConstant> vpc = isValuePlusConstant(idx);
      if (vpc.has_value()) {
        // GEP index is `vpc->value` plus `vpc->constant`. Let's see if
        // `vpc->value` is itself an induction variable. This can happen if we
        // are, say, accessing `arr[k+1]` in a loop over `k`
        iv = isInductionVar(vpc->value);
        if (iv.has_value()) {
          LLVM_DEBUG(dbgs() << "DMS:     GEP single index is an induction var plus a constant " << vpc->constant << "\n");
          iv->initial_val = iv->initial_val + vpc->constant;
          // the first iteration, it's the initial value of the induction variable
          // plus the constant it's always modified by. but the induction increment
          // doesn't care about the constant modification
        }
      }
    }
    if (!iv.has_value()) {
      LLVM_DEBUG(dbgs() << "DMS:     not an induction pattern\n");
      return no_induction_pattern;
    }
    // If we get to here, we've found an induction pattern, described by
    // `ivr.initial_val` and `ivr.induction_increment`.
    // However, we still need to ensure that the pointer produced by the GEP
    // actually is guaranteed to be dereferenced during every iteration
    // (resetting it to clean) -- otherwise we can't use the induction reasoning.
    // For now, we conservatively require a stronger property:
    //   - the pointer must be used for one or more load/stores
    //   - at least one of those load/stores must have both of these properties:
    //     - postdominates the GEP
    //     - is inside the loop
    bool success = false;
    const Loop* geploop = loopinfo.getLoopFor(gep.getParent());
    if (!geploop) {
      LLVM_DEBUG(dbgs() << "DMS:     expected GEP to be in a loop, but it's not... weird\n");
      return no_induction_pattern;
    }
    for (const User* user : gep.users()) {
      if (isa<LoadInst>(user) || isa<StoreInst>(user)) {
        const Instruction* inst = cast<Instruction>(user);
        // first check: the load or store postdominates the GEP
        if (!pdtree.dominates(inst, &gep)) {
          assert(inst->getParent() != gep.getParent());
          continue;
        }
        // second check: the load or store must execute during each loop iteration.
        // I.e., the load or store must execute between each two consecutive
        // executions of the GEP.
        // I.e., the load or store is present on all paths from the GEP to itself.
        // For now, we approximate this as, the load/store and the GEP have the same
        // result for "what is the innermost loop you live in". I think that works.
        // At least, it works for our current regression tests.
        if (loopinfo.getLoopFor(inst->getParent()) == geploop) {
          success = true;
          break;
        }
      }
    }
    if (success) {
      // we have the constant initial_val and induction_increment.
      // but we still need to scale them by the size of the underlying array
      // elements, in order to get the GEP offsets.
      auto element_size = DL.getTypeStoreSize(gep.getSourceElementType()).getFixedSize();
      assert(element_size > 0);
      APInt ap_element_size = APInt(/* bits = */ 64, /* val = */ element_size);
      InductionPatternResult ipr;
      ipr.is_induction_pattern = true;
      ipr.initial_offset = iv->initial_val * ap_element_size;
      ipr.induction_offset = iv->induction_increment * ap_element_size;
      LLVM_DEBUG(dbgs() << "DMS:     induction pattern with initial " << ipr.initial_offset << " and induction " << ipr.induction_offset << "\n");
      return ipr;
    } else {
      LLVM_DEBUG(dbgs() << "DMS:     but failed the dereference-inside-loop check\n");
      return no_induction_pattern;
    }
  }
  llvm_unreachable("should return from inside the for loop");
}

/// Is the given `val` an induction variable?
/// Here, "induction variable" is narrowly defined as:
///     a PHI between a constant (initial value) and a variable (induction)
///     equal to itself plus or minus a constant
///
/// If so, return the `InductionVar`; otherwise, return nothing
static std::optional<InductionVar> isInductionVar(const Value* val) {
  if (const PHINode* phi = dyn_cast<PHINode>(val)) {
    bool found_initial_val = false;
    bool found_induction_increment = false;
    APInt initial_val;
    APInt induction_increment;
    for (const Use& use : phi->incoming_values()) {
      const Value* phi_val = use.get();
      if (const ConstantInt* phi_val_constint = dyn_cast<ConstantInt>(phi_val)) {
        if (found_initial_val) {
          // two constants in this phi. For now, this isn't a pattern we'll consider for induction.
          return std::nullopt;
        }
        found_initial_val = true;
        initial_val = phi_val_constint->getValue();
      } else {
        const std::optional<ValPlusConstant> vpc = isValuePlusConstant(phi_val);
        if (vpc.has_value()) {
          if (found_induction_increment) {
            // two non-constants in this phi. For now, this isn't a pattern we'll consider for induction.
            return std::nullopt;
          }
          // we're looking for the case where we are adding or subbing a
          // constant from the same value
          if (vpc->value == val) {
            found_induction_increment = true;
            induction_increment = vpc->constant;
          }
        }
      }
    }
    if (found_initial_val && found_induction_increment) {
      initial_val = initial_val.sextOrSelf(64);
      induction_increment = induction_increment.sextOrSelf(64);
      LLVM_DEBUG(dbgs() << "DMS:     Found an induction var, initial " << initial_val << " and induction " << induction_increment << "\n");
      InductionVar iv;
      iv.is_induction_var = true;
      iv.initial_val = std::move(initial_val);
      iv.induction_increment = std::move(induction_increment);
      return iv;
    } else {
      return std::nullopt;
    }
  } else {
    return std::nullopt;
  }
}

/// Is the given `val` defined as some other `Value` plus/minus a constant?
///
/// If so, return the `ValPlusConstant`; otherwise, return nothing
static std::optional<ValPlusConstant> isValuePlusConstant(const Value* val) {
  if (const BinaryOperator* bop = dyn_cast<BinaryOperator>(val)) {
    switch (bop->getOpcode()) {
      case Instruction::Add:
      case Instruction::Sub:
      {
        bool found_constant_operand = false;
        bool found_nonconstant_operand = false;
        APInt constant_val;
        const Value* nonconstant_val;
        for (const Value* op : bop->operand_values()) {
          if (const ConstantInt* op_const = dyn_cast<ConstantInt>(op)) {
            if (found_constant_operand) {
              // adding or subbing two constants. Shouldn't be valid LLVM, but we'll fail gracefully.
              return std::nullopt;
            }
            found_constant_operand = true;
            constant_val = op_const->getValue();
            if (bop->getOpcode() == Instruction::Sub) {
              constant_val = -constant_val;
            }
          } else {
            if (found_nonconstant_operand) {
              // two nonconstant operands
              return std::nullopt;
            }
            found_nonconstant_operand = true;
            nonconstant_val = op;
          }
        }
        if (found_constant_operand && found_nonconstant_operand) {
          constant_val = constant_val.sextOrSelf(64);
          ValPlusConstant vpc;
          vpc.value = std::move(nonconstant_val);
          vpc.constant = std::move(constant_val);
          return vpc;
        } else {
          return std::nullopt;
        }
      }
      default: {
        return std::nullopt;
      }
    }
  } else {
    return std::nullopt;
  }
}
