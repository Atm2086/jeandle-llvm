//===- JavaType.h - Java Type Query Interface -----------------------------===//
//
// Copyright (c) 2025, the Jeandle-LLVM Authors. All Rights Reserved.
//
// Part of the Jeandle-LLVM project, under the Apache License v2.0 with LLVM
// Exceptions. See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Provides interfaces to query Java type information from LLVM IR.
// Type information is encoded in attributes ("java-klass", "java-klass-exact")
// and metadata (!java-klass). This interface can be used by any Jeandle pass
// that needs compile-time type information.
//
//===----------------------------------------------------------------------===//

#ifndef JEANDLE_JAVA_TYPE_H
#define JEANDLE_JAVA_TYPE_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Compiler.h"

#include <cstdint>

namespace llvm {

class BasicBlock;
class DominatorTree;
class Instruction;
class Value;

namespace jeandle {

/// Represents the Java type of an LLVM IR value, including both positive
/// type knowledge and negative constraints (excluded classes).
///
/// JavaType does not encode nullability. A known Klass describes the Java
/// class of the referenced object when the queried value is known to denote a
/// non-null oop.
struct JavaType {
  /// The Klass pointer (from HotSpot JVM). 0 means unknown.
  uintptr_t Klass = 0;

  SmallDenseSet<uintptr_t, 2> Interfaces;

  /// If true, the value is exactly this class, not a subclass.
  bool Exact = false;

  /// Klasses this value is known NOT to be an instance of.
  /// Populated from failed type checks (type-denied branch outcomes) met
  /// along the CFG paths that reach the query point and joined at merges —
  /// the denying check need not dominate the query point. Only the most
  /// general (uppermost) excluded classes are stored; more specific subtypes
  /// are implied.
  SmallDenseSet<uintptr_t, 2> ExcludedKlasses;
  JavaType() = default;
  JavaType(uintptr_t Klass, bool Exact);
  bool isUnknown() const {
    return Klass == 0 && Interfaces.empty() && ExcludedKlasses.empty();
  }
  bool isKnown() const { return Klass != 0; }
  bool hasExclusions() const { return !ExcludedKlasses.empty(); }

  bool operator==(const JavaType &Other) const {
    return Klass == Other.Klass && Exact == Other.Exact &&
           ExcludedKlasses == Other.ExcludedKlasses &&
           Interfaces == Other.Interfaces;
  }

  bool operator!=(const JavaType &Other) const { return !(*this == Other); }
};

/// Oracle answering "is V provably null on the CFG edge FromBB->ToBB?".
/// Edge-semantics sharpening skips proven-null edges: JavaType is conditioned
/// on a non-null oop (see struct JavaType above), and a null-proven edge
/// carries no non-null type constraint. Only positive proof may return true;
/// when in doubt, return false (keeping the edge is always sound).
/// The standard implementation is LVINullEdgeOracle (LazyValueInfo-backed);
/// it recognizes null tests both directly on the edge's branch and through
/// threaded/indirect forms. Context-sensitive callers must supply one; a null
/// oracle is only meaningful for context-insensitive (base-only) queries,
/// where the engine is not run at all.
using IsNullEdgeOracle =
    function_ref<bool(Value *V, BasicBlock *FromBB, BasicBlock *ToBB)>;

/// Nullness of a value at the top of a block.
enum class Nullness {
  /// No conclusion: nothing proves the value null or non-null there.
  Unknown,
  /// Proven non-null there.
  NonNull,
  /// Proven null there.
  Null,
};

/// Structural nullness of V at the top of From, derived from dominance only.
///
/// The scan walks the dominator chain of From up to (and including) the block
/// that defines V, looking for a conditional branch on `icmp eq/ne V, null`:
/// when the null side of that branch dominates From, V is null at From; when
/// the non-null side dominates From, V is not null there.  A branch whose two
/// successors are the same block proves nothing (the edge is a duplicate), and
/// two tests that contradict each other prove nothing either.
///
/// The conclusion is a proof and therefore holds on every edge out of From: V
/// is an SSA value, so its null-ness cannot change inside the block.  Sharing
/// one implementation between the cached and the reference engine keeps the
/// two engines on the same null-edge semantics.
LLVM_ABI Nullness nullnessAtBlockEnd(Value *V, BasicBlock *From,
                                     DominatorTree &DT);

/// Negative pre-filter for the null-edge oracle.
///
/// The oracle answers "is V provably null on the edge From->To?".  It can only
/// answer "yes" in two situations:
///
///  (1) The lattice folds V itself to the constant null.  LazyValueInfo folds
///      a pointer to null only through a `select` whose arms are both null, a
///      `phi` all of whose incoming arms are null on their edge, or a null
///      constant; every other pointer definition is overdefined (or provably
///      non-null) and can never become a null constant.
///
///  (2) A condition that dominates the edge tests V against null: an
///      `icmp eq/ne V, null` on the edge's own terminator, on a dominating
///      block's terminator, or an assume/guard constraining V.
///
/// The filter answers "no proof is possible" when neither can hold.  It is a
/// refutation, not a proof: it never skips an edge the oracle would have
/// skipped, so it can cost precision but never soundness.  Keeping an edge is
/// always sound (JavaType is conditioned on a non-null oop); skipping an edge
/// that is not proven null is not.
///
/// One instance per top-level query: the per-value data (canonical value,
/// null-test blocks, fold conclusion) is memoized, so a value is analysed once
/// per query no matter how many edges are examined.
class NullEdgePreFilter {
public:
  /// Block-level state of a value at the top of a block.
  ///
  /// Both answers come out of a single walk of the dominator chain, so the
  /// chain is traversed once per (value, block) pair instead of once per
  /// question.
  struct BlockState {
    /// Three-valued conclusion derived from dominance only: Null when the
    /// null side of a dominating null test reaches the block, NonNull when
    /// the non-null side does, Unknown otherwise.
    Nullness Structural = Nullness::Unknown;
    /// Whether the dominator chain contains a block in which the value is
    /// compared against null. This is the question the negative filter asks:
    /// without such a test no lattice query can prove the value null.
    bool HasNullTest = false;
  };

  /// Computes the block-level state of V at the top of From.
  ///
  /// The walk stops at the block that defines V: above its definition the
  /// value does not exist yet, so no test can mention it.
  BlockState blockState(Value *V, BasicBlock *From, DominatorTree &DT);

  /// Returns true when a null proof for V on an edge out of From is possible.
  /// When it returns false the caller must not ask the oracle.
  /// \p State must be the block state of the same (V, From) pair.
  bool mayProveNull(Value *V, const BlockState &State, DominatorTree &DT);

private:
  /// The value the oracle reasons about: pointer casts and aliases stripped,
  /// and a freeze stripped only when its operand is proven defined.  Mirrors
  /// the normalization done by LVINullEdgeOracle.
  Value *canonicalize(Value *V);

  /// Blocks in which V is compared against null, memoized per value.
  const SmallPtrSet<BasicBlock *, 8> &getNullTestBlocks(Value *V);

  /// Can the lattice fold V to the constant null?  Conservative: an answer of
  /// false means V provably cannot become a null constant.
  bool mayFoldToNull(Value *V, unsigned Depth);

  /// Can the incoming value X be null on the edge out of PhiBB?
  bool mayBeNullOnEdge(Value *X, BasicBlock *PhiBB, unsigned Depth);

  /// Is V compared against null in block B (directly, through pointer casts or
  /// under a not/and/or condition)?
  bool isNullTestedInBlock(Value *V, BasicBlock *B);

  /// Does a block dominating B (up to V's definition block) test V against
  /// null?
  bool hasDominatingNullTest(Value *V, BasicBlock *B);

  /// Blocks in which V is compared against null, memoized per value.
  DenseMap<Value *, SmallPtrSet<BasicBlock *, 8>> NullTestBlocks;
  /// Whether the lattice may fold the value to the constant null, memoized.
  DenseMap<Value *, bool> MayFoldNull;
  /// The canonical value the oracle reasons about, memoized per value.
  DenseMap<Value *, Value *> CanonicalValues;
  /// The dominator tree of the running query (the filter is per-query).
  DominatorTree *DT = nullptr;
};

/// Counters of the null-edge pre-filter.
///   Hits - queries answered "no proof possible" (the oracle was not asked)
///   Uses - V users scanned while building the mentioning-block sets
struct NullEdgePreFilterStats {
  uint64_t Hits = 0;
  uint64_t Uses = 0;
};

/// Per-query budget for null-edge oracle calls.
///
/// The oracle asks LazyValueInfo whether a value is provably null on an edge.
/// That query is the dominant cost of the path-facts join, so one top-level
/// query may only ask it a bounded number of times. Exceeding the budget is
/// always sound: the oracle is not asked and the answer is "not proven null",
/// i.e. the edge is kept — keeping an edge never removes a type constraint.
///
/// The budget is deliberately *not* a precision knob: it trades precision for
/// a compile-time bound, and it is reported so that the differential
/// verification does not require two evaluations to agree when they were
/// truncated at different points.
class NullEdgeOracleBudget {
public:
  explicit NullEdgeOracleBudget(unsigned Limit) : Remaining(Limit) {}

  /// Reserves one oracle call. Returns false when the budget is spent, in
  /// which case the caller must not ask the oracle.
  LLVM_ABI bool consume();

  /// Whether the budget was spent during this query.
  bool exhausted() const { return Spent; }

  /// Number of oracle calls this budget refused.
  uint64_t skippedCalls() const { return Skipped; }

  /// Number of oracle calls this budget granted.
  uint64_t grantedCalls() const { return Granted; }

private:
  unsigned Remaining;
  bool Spent = false;
  uint64_t Skipped = 0;
  uint64_t Granted = 0;
};

/// Counters of the null-edge oracle budget.
///   ExhaustedQueries - top-level queries that hit the budget
///   SkippedCalls - oracle calls that were not made because the budget was
///                  already spent
///   MaxCallsPerQuery - the largest number of oracle calls one query made;
///                  this is the distribution the default budget is derived from
struct NullEdgeOracleBudgetStats {
  uint64_t ExhaustedQueries = 0;
  uint64_t SkippedCalls = 0;
  uint64_t MaxCallsPerQuery = 0;
};

/// Returns the process-wide null-edge oracle budget counters.
LLVM_ABI NullEdgeOracleBudgetStats getNullEdgeOracleBudgetStats();

/// Records the outcome of one top-level query: `Granted` oracle calls were
/// made, `Skipped` calls were refused because the budget was already spent,
/// and the budget was exhausted when `Exhausted` is set.
///
/// The per-thread exhaustion counter is monotonic on purpose: the
/// differential verification compares it before and after a fresh evaluation
/// to detect that the two evaluations were truncated at different points, in
/// which case their results need not be equal.
LLVM_ABI void noteNullEdgeOracleBudgetQuery(uint64_t Granted, uint64_t Skipped,
                                            bool Exhausted);

/// Returns the number of top-level queries on this thread that hit the
/// null-edge oracle budget.
LLVM_ABI uint64_t getNullEdgeOracleBudgetExhaustionCount();

/// Returns the configured per-query null-edge oracle call budget.
///
/// The option is owned by the reference engine (the IR layer); the cached
/// engine reads it through this accessor so that both engines spend the same
/// budget. That equality is what makes a differential comparison meaningful:
/// the two engines must truncate at comparable points, otherwise they answer
/// different questions.
LLVM_ABI unsigned getNullEdgeOracleCallBudget();

/// Flushes this thread's null-edge pre-filter counters into the process-wide
/// totals and resets them. Called once per top-level Java type query, so the
/// filter's hot path never touches a shared atomic.
LLVM_ABI void flushNullEdgePreFilterStats();

/// Returns the process-wide null-edge pre-filter counters.
LLVM_ABI NullEdgePreFilterStats getNullEdgePreFilterStats();

/// Get the Java type of a value.
///
/// When Context is null, performs a context-insensitive query using only
/// attributes and metadata attached to the value and supported dependencies
/// (PHIs, selects and pointer-identity expressions). Returned arguments and
/// explicit return attributes constrain the result simultaneously.
///
/// When Context is provided, additionally performs context-sensitive sharpening
/// with the edge-semantics dataflow engine: guards attached to CFG edges are
/// met along paths and joined at merges, giving the type constraints that hold
/// for V at the context instruction. Proven-null edges and back edges are
/// excluded; both exclusions are exact under the non-null contract (see the
/// design comment in JavaType.cpp). Null-edge proofs come from the supplied
/// oracle.
///
/// JavaType does not model nullability. Sharpening derived from
/// jeandle.check_instanceof is therefore only sound for consumers whose IR/API
/// contract guarantees that the queried oop is non-null at the check site
/// (e.g. check_instanceof's nonnull parameter, or an explicit non-null proof
/// as in ConstantFieldFolding). Edges on which the value is provably null are
/// excluded from the analysis via the supplied oracle — including individual
/// PHI incoming arms — so a PHI whose remaining arms agree on a klass can
/// come back Known even though the value can be null on the skipped arm.
/// Consumers that materialize the result as IR facts (metadata, assumes) must
/// themselves ensure the value is non-null at the point of use, as the IR
/// semantics of that use (a field load's implicit null check, a virtual
/// call's receiver dispatch) normally do.
///
/// The query traces through a limited set of IR patterns:
/// - PHI nodes (with cycle detection for loop back-edges)
/// - Select instructions (constant and non-constant arms)
/// - Integer casts: zext, sext, trunc (but not bitcast, fpcast, etc.)
/// - ICmp comparisons of a check_instanceof result against a constant
/// - Switch case values of a check_instanceof result
/// - Direct jeandle.check_exact_klass calls
/// - Freeze of a traced condition (with a definedness proof)
/// - And (i1) of two traced conditions
/// - Or (i1) of two traced conditions
/// - Xor i1 %a, true: logical NOT
/// - Direct jeandle.check_instanceof calls
///
/// Widened boolean forms are tracked by their exact value domain, not by an
/// assumed {0,1}: sext of an i1 check result widens to {0,-1}, and constant
/// arms equal to neither domain value make the expression check-independent.
/// And/Or/Xor are matched on i1 only. Unrecognized patterns conservatively
/// return unknown ({}).
JavaType getJavaType(Value *V, DominatorTree *DT, Instruction *Context,
                     IsNullEdgeOracle IsNullEdge);

/// Compute the type constraints that CFG edge guards imply for V at Context.
/// Returns ONLY check-derived constraints (a positive klass when a passing
/// check proves it, with Exact set from IsEffectivelyFinal, plus excluded
/// klasses from failed checks) — never attribute/metadata-derived base types.
///
/// DestBB must be the PHI's parent block and Context the terminator of the
/// PHI incoming block: the guard on the edge Context->parent -> DestBB is
/// also considered, because a PHI incoming use is edge-local — if exactly one
/// successor is DestBB, that outcome's constraints apply to the value flowing
/// along that edge.
///
/// The result depends only on the CFG, branch conditions, the null-edge
/// oracle and VM callbacks; it is constant while those are unchanged. Sound
/// under the same non-null oop contract as getJavaType.
JavaType sharpen(Value *V, Instruction *Context, DominatorTree &DT,
                 BasicBlock *DestBB, IsNullEdgeOracle IsNullEdge);

/// Compute the type union of two Java types. Used when the value could be
/// either type (PHI, select). Widens positive type to LCA and keeps exclusions
/// implied by both operands, explicitly or through positive bounds.
JavaType typeUnion(JavaType A, JavaType B);

/// Compute the type intersection of two Java types. Used when both constraints
/// apply simultaneously to the same value (base type + sharpened type).
/// Narrows to the more specific positive type and unions ExcludedKlasses.
JavaType typeIntersect(JavaType A, JavaType B);

/// Returns true if Klass and OtherKlass are provably incompatible, including
/// Java's interface and array-covariance rules. Requires VM callbacks to be
/// registered. Missing interface-array classification conservatively prevents
/// proofs of incompatibility between unrelated non-exact class bounds.
/// \p KlassExact indicates whether Klass is known to be an exact type.
bool areKlassesIncompatible(uintptr_t Klass, bool KlassExact,
                            uintptr_t OtherKlass);

/// Extract a Klass pointer constant from a Value.
/// Strips pointer casts and aliases first, then handles: defined freeze
/// operands, inttoptr of ConstantInt (instruction and ConstantExpr forms),
/// inttoptr(zext/sext(V)) widening casts, inttoptr(ptrtoint(V)) round-trip
/// chains, load from a constant GlobalVariable (recurses into initializer,
/// follows GlobalAlias), and bare ConstantInt (reachable via GlobalVariable
/// recursion). Has a recursion depth limit to prevent infinite recursion.
/// Returns 0 if the value does not encode a constant klass pointer.
uintptr_t extractKlassConstant(Value *V);

/// Return the single input of a Java pointer-identity expression, or nullptr.
/// Recognizes zero-index GEPs, non-interposable aliases, pointer casts,
/// invariant-group intrinsics and returned-argument calls. An independent
/// result type constrains this dependency simultaneously. Freeze is handled
/// separately because forwarding through it requires a definedness proof.
Value *getJavaIdentityInput(Value *V);

/// Prove that V is defined (never poison) at its definition, without
/// context. Recognized as defined: freeze/alloca results, constants,
/// arguments and call results carrying noundef/dereferenceable attributes,
/// loads with the matching metadata, and the jeandle check helpers (their
/// templates compute a concrete boolean for every contract-valid input), plus
/// a closed set of pure operators over defined inputs. Anything else —
/// including operations with poison-generating flags and addrspacecast — is
/// conservatively not proven. This is the proof JavaType itself requires
/// before forwarding type facts through a freeze; passes that mirror the
/// freeze/definedness discipline (e.g. RecoverTypeInfo's lattice) should use
/// the same predicate rather than re-deriving one.
bool isProvenDefined(Value *V);

} // namespace jeandle
} // namespace llvm

#endif // JEANDLE_JAVA_TYPE_H
