#include "llvm/ADT/APInt.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"

extern const llvm::APInt zero;
extern const llvm::APInt minusone;

namespace llvm {

/// Holds the bounds information for a single pointer, if it is known.
/// Unlike the PointerStatus, which can be different at different program
/// points for the same pointer, the BoundsInfo is the same at all program
/// points for a given pointer.
class BoundsInfo final {
public:
	enum Kind {
		/// Bounds info is not known for this pointer. Dereferencing this
		/// pointer is a compile-time error - we should know bounds info for
		/// all pointers which are ever dereferenced.
		UNKNOWN = 0,
		/// Bounds info is known statically. Get it with `static_info()`
		STATIC,
		/// Bounds info is known dynamically. Get it with `dynamic_info()`
		DYNAMIC,
		/// Bounds info is known dynamically, and is derived as the merger of the
		/// bounds infos in `.merge_inputs`. Get it with `dynamic_info()`
		DYNAMIC_MERGED,
		/// This pointer should be considered to have infinite bounds in both
		/// directions
		INFINITE,
	};

	Kind get_kind() const {
		return kind;
	}

	/// Statically known bounds info.
	///
	/// Suppose the pointer value is P. If the `low_offset` is L and the
	/// `high_offset` is H, that means we know that the allocation extends at
	/// least from (P - L) to (P + H), inclusive.
	/// Notes:
	///   - `low_offset` and `high_offset` are in bytes.
	///   - In the common case (where we know P is inbounds) `low_offset` and
	///     `high_offset` will both be nonnegative. `low_offset` is implicitly
	///     subtracted, as noted above.
	///   - Values of 0 in both fields indicates a pointer valid for exactly the
	///     single byte it points to.
	///   - Greater values in either of these fields lead to more permissive
	///     (larger) bounds.
	class StaticBoundsInfo final {
	public:
		APInt low_offset;
		APInt high_offset;

		explicit StaticBoundsInfo(APInt low_offset, APInt high_offset)
			: low_offset(low_offset), high_offset(high_offset) {}
		StaticBoundsInfo() : low_offset(zero), high_offset(minusone) {}

		bool operator==(const StaticBoundsInfo& other) const {
			return (low_offset == other.low_offset && high_offset == other.high_offset);
		}
		bool operator!=(const StaticBoundsInfo& other) const {
			return !(*this == other);
		}

		/// `cur_ptr`: the pointer value for which these static bounds apply.
		///
		/// `Builder`: the IRBuilder to use to insert dynamic instructions.
		///
		/// `bounds_insts`: If we insert any instructions into the program, we'll
		/// also add them to `bounds_insts`, see notes there
		///
		/// Returns the "base" (minimum inbounds pointer value) of the allocation,
		/// as an LLVM `Value` of type `i8*`.
		Value* base_as_llvm_value(
			Value* cur_ptr,
			IRBuilder<>& Builder,
			DenseSet<const Instruction*>& bounds_insts
		) const {
			return add_offset_to_ptr(cur_ptr, -low_offset, Builder, bounds_insts);
		}

		/// `cur_ptr`: the pointer value for which these static bounds apply.
		///
		/// `Builder`: the IRBuilder to use to insert dynamic instructions.
		///
		/// `bounds_insts`: If we insert any instructions into the program, we'll
		/// also add them to `bounds_insts`, see notes there
		///
		/// Returns the "max" (maximum inbounds pointer value) of the allocation,
		/// as an LLVM `Value` of type `i8*`.
		Value* max_as_llvm_value(
			Value* cur_ptr,
			IRBuilder<>& Builder,
			DenseSet<const Instruction*>& bounds_insts
		) const {
			return add_offset_to_ptr(cur_ptr, high_offset, Builder, bounds_insts);
		}
	};

	/// Represents a pointer value as an LLVM pointer, with an optional
	/// constant offset.
	///
	/// The reason we represent it this way, rather than directly as a
	/// LLVM Value (eg an LLVM GEP on a cast of `ptr`), is that this
	/// representation is easier to compare for equality.
	struct PointerWithOffset {
		/// Pointer value itself.
		/// This can be a `Value` of any pointer type.
		Value* ptr;
		/// Offset, in bytes.
		/// This can be positive, negative, or zero.
		APInt offset;

		/// Get the pointer value as an LLVM `Value` of type `i8*`.
		///
		/// `bounds_insts`: If we insert any instructions into the program, we'll
		/// also add them to `bounds_insts`, see notes there
		Value* as_llvm_value(IRBuilder<>& Builder, DenseSet<const Instruction*>& bounds_insts) const {
			return add_offset_to_ptr(ptr, offset, Builder, bounds_insts);
		}

		PointerWithOffset() : ptr(NULL), offset(zero) {}
		PointerWithOffset(Value* ptr) : ptr(ptr), offset(zero) {}
		PointerWithOffset(Value* ptr, APInt offset) : ptr(ptr), offset(offset) {}
		PointerWithOffset(Value* ptr, uint64_t offset) : ptr(ptr), offset(APInt(/* bits = */ 64, /* val = */ offset)) {}

		bool operator==(const PointerWithOffset& other) const {
			return (ptr == other.ptr && offset == other.offset);
		}
		bool operator!=(const PointerWithOffset& other) const {
			return !(*this == other);
		}
	};

	/// Dynamically known bounds info
	struct DynamicBoundsInfo {
		/// Pointer value representing the beginning of the allocation
		/// (i.e., the minimum valid pointer value)
		PointerWithOffset base;
		/// Pointer value representing the end of the allocation
		/// (i.e., the maximum valid pointer value)
		PointerWithOffset max;

		explicit DynamicBoundsInfo(Value* base, Value* max)
			: base(PointerWithOffset(base)), max(PointerWithOffset(max)) {}
		explicit DynamicBoundsInfo(PointerWithOffset base, PointerWithOffset max)
			: base(base), max(max) {}
		DynamicBoundsInfo() : base(PointerWithOffset()), max(PointerWithOffset()) {}

		bool operator==(const DynamicBoundsInfo& other) const {
			return (base == other.base && max == other.max);
		}
		bool operator!=(const DynamicBoundsInfo& other) const {
			return !(*this == other);
		}
	};

	/// Get the StaticBoundsInfo, or NULL if not `is_static()`
	const StaticBoundsInfo* static_info() const {
		if (is_static()) {
			return &info.static_info;
		} else {
			return NULL;
		}
	}

	/// Get the DynamicBoundsInfo, or NULL if not `is_dynamic()`
	const DynamicBoundsInfo* dynamic_info() const {
		if (is_dynamic()) {
			return &info.dynamic_info;
		} else {
			return NULL;
		}
	}

	/// Helper, returns `true` if the kind is `STATIC`.
	/// If this returns `true`, `static_info()` will return non-NULL
	bool is_static() const {
		return (kind == STATIC);
	}

	/// Helper, returns `true` if the kind is `DYNAMIC` or `DYNAMIC_MERGED`.
	/// In either case, if this returns `true`, `dynamic_info()` will return
	/// non-NULL
	bool is_dynamic() const {
		return (kind == DYNAMIC || kind == DYNAMIC_MERGED);
	}

	/// Only valid if kind is `DYNAMIC_MERGED`.
	/// Elements in this are new()'d - they must be delete()'d.
	/// Elements in this will never be of kind `DYNAMIC_MERGED` themselves.
	SmallVector<BoundsInfo*, 4> merge_inputs;

	/// Construct a BoundsInfo with the given `StaticBoundsInfo`
	explicit BoundsInfo(StaticBoundsInfo static_info) :
		kind(STATIC), info(static_info) {}

	/// Construct a BoundsInfo with the given static `low_offset` and
	/// `high_offset`
	static BoundsInfo static_bounds(APInt low_offset, APInt high_offset) {
		return BoundsInfo(StaticBoundsInfo(low_offset, high_offset));
	}

	/// Construct a BoundsInfo with the given `DynamicBoundsInfo`
	explicit BoundsInfo(DynamicBoundsInfo dynamic_info) :
		kind(DYNAMIC), info(dynamic_info) {}

	/// Construct a BoundsInfo with the given dynamic `base` and `max`
	static BoundsInfo dynamic_bounds(Value* base, Value* max) {
		return BoundsInfo(DynamicBoundsInfo(base, max));
	}

	/// Construct a BoundsInfo with the given dynamic `base` and `max`
	static BoundsInfo dynamic_bounds(PointerWithOffset base, PointerWithOffset max) {
		return BoundsInfo(DynamicBoundsInfo(base, max));
	}

	/// Construct a BoundsInfo with infinite bounds in both directions
	static BoundsInfo infinite() {
		BoundsInfo ret;
		ret.kind = INFINITE;
		return ret;
	}

	/// Construct a BoundsInfo with unknown bounds. This is the default when
	/// constructing a BoundsInfo. A zero-argument constructor seems to be
	/// required for value types of DenseMap
	explicit BoundsInfo() : kind(UNKNOWN), info(StaticBoundsInfo()) {}

	/// Construct a BoundsInfo with unknown bounds (alias for `BoundsInfo()`)
	static BoundsInfo unknown_bounds() {
		return BoundsInfo();
	}

	BoundsInfo(const BoundsInfo& other) :
		kind(other.kind), info(other.info) {
		// separately new() for each BoundsInfo in merge_inputs.
		// That way, if `other` is destructed before this new copy,
		// it doesn't delete() the merge_inputs we're using
		for (BoundsInfo* binfo : other.merge_inputs) {
			merge_inputs.push_back(new BoundsInfo(*binfo));
		}
	}
	// https://stackoverflow.com/questions/3652103/implementing-the-copy-constructor-in-terms-of-operator
	// https://stackoverflow.com/questions/3279543/what-is-the-copy-and-swap-idiom
	friend void swap(BoundsInfo& A, BoundsInfo& B) noexcept {
		std::swap(A.kind, B.kind);
		std::swap(A.info, B.info);
		std::swap(A.merge_inputs, B.merge_inputs);
	}
	BoundsInfo(BoundsInfo&& other) noexcept : BoundsInfo() {
		swap(*this, other);
	}
	BoundsInfo& operator=(BoundsInfo rhs) noexcept {
		swap(*this, rhs);
		return *this;
	}
	~BoundsInfo() {
		for (BoundsInfo* binfo : merge_inputs) {
			delete binfo;
		}
	}

	// operator== and operator!= intentionally ignore the .merge_inputs field
	// here
	bool operator==(const BoundsInfo& other) const {
		if (kind != other.kind) return false;
		switch (kind) {
			case UNKNOWN:
			case INFINITE:
				return true;
			case STATIC:
				return (info.static_info == other.info.static_info);
			case DYNAMIC:
			case DYNAMIC_MERGED:
				return (info.dynamic_info == other.info.dynamic_info);
			default:
				llvm_unreachable("Missing BoundsInfo.kind case");
		}
	}
	bool operator!=(const BoundsInfo& other) const {
		return !(*this == other);
	}

	/// `cur_ptr`: the pointer value for which these bounds apply.
	///
	/// `Builder`: the IRBuilder to use to insert dynamic instructions.
	///
	/// `bounds_insts`: If we insert any instructions into the program, we'll
	/// also add them to `bounds_insts`, see notes there
	///
	/// Returns the "base" (minimum inbounds pointer value) of the allocation,
	/// as an LLVM `Value` of type `i8*`.
	/// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
	Value* base_as_llvm_value(
		Value* cur_ptr,
		IRBuilder<>& Builder,
		DenseSet<const Instruction*>& bounds_insts
	) const;

	/// `cur_ptr`: the pointer value for which these bounds apply.
	///
	/// `Builder`: the IRBuilder to use to insert dynamic instructions.
	///
	/// `bounds_insts`: If we insert any instructions into the program, we'll
	/// also add them to `bounds_insts`, see notes there
	///
	/// Returns the "max" (maximum inbounds pointer value) of the allocation,
	/// as an LLVM `Value` of type `i8*`.
	/// Or, if the `kind` is UNKNOWN or INFINITE, returns NULL.
	Value* max_as_llvm_value(
		Value* cur_ptr,
		IRBuilder<>& Builder,
		DenseSet<const Instruction*>& bounds_insts
	) const;

	/// `cur_ptr` is the pointer which these bounds are for.
	///
	/// `Builder` is the IRBuilder to use to insert dynamic instructions, if
	/// that is necessary.
	///
	/// `bounds_insts`: If we insert any instructions into the program, we'll
	/// also add them to `bounds_insts`, see notes there
	static BoundsInfo merge(
		const BoundsInfo& A,
		const BoundsInfo& B,
		Value* cur_ptr,
		IRBuilder<>& Builder,
		DenseSet<const Instruction*>& bounds_insts
	);

private:
	/// This should really be a union.  But C++ complains hard about implicitly
	/// deleted copy constructors, implicitly deleted assignment operators, etc
	/// and I'm not C++ enough to be able to fix it
	struct StaticOrDynamic {
		StaticBoundsInfo static_info;
		DynamicBoundsInfo dynamic_info;

		StaticOrDynamic(StaticBoundsInfo static_info) : static_info(static_info) {}
		StaticOrDynamic(DynamicBoundsInfo dynamic_info) : dynamic_info(dynamic_info) {}
	};

	Kind kind;
	StaticOrDynamic info;

	/// Adds the given `offset` (in _bytes_) to the given `ptr`, and returns
	/// the resulting pointer.
	/// The input pointer can be any pointer type, the output pointer will
	/// have type `i8*`.
	///
	/// `Builder`: the IRBuilder to use to insert dynamic instructions.
	///
	/// `bounds_insts`: If we insert any instructions into the program, we'll
	/// also add them to `bounds_insts`, see notes there
	static Value* add_offset_to_ptr(
		Value* ptr,
		APInt offset,
		IRBuilder<>& Builder,
		DenseSet<const Instruction*>& bounds_insts
	);

	/// `cur_ptr` is the pointer which these bounds are for.
	///
	/// `Builder` is the IRBuilder to use to insert dynamic instructions.
	///
	/// `bounds_insts`: If we insert any instructions into the program, we'll
	/// also add them to `bounds_insts`, see notes there
	static BoundsInfo merge_static_dynamic(
		const StaticBoundsInfo& static_info,
		const DynamicBoundsInfo& dynamic_info,
		Value* cur_ptr,
		IRBuilder<>& Builder,
		DenseSet<const Instruction*>& bounds_insts
	);

	/// `Builder` is the IRBuilder to use to insert dynamic instructions.
	///
	/// `bounds_insts`: If we insert any instructions into the program, we'll
	/// also add them to `bounds_insts`, see notes there
	static BoundsInfo merge_dynamic_dynamic(
		const DynamicBoundsInfo& a_info,
		const DynamicBoundsInfo& b_info,
		IRBuilder<>& Builder,
		DenseSet<const Instruction*>& bounds_insts
	);
};

} // end namespace
