//===-- gen/trycatchfinally.h - Try/catch/finally scopes --------*- C++ -*-===//
//
//                         LDC � the LLVM D compiler
//
// This file is distributed under the BSD-style LDC license. See the LICENSE
// file for details.
//
//===----------------------------------------------------------------------===//

#ifndef LDC_GEN_TRYCATCHFINALLY_H
#define LDC_GEN_TRYCATCHFINALLY_H

#include "globals.h"
#include <stddef.h>
#include <vector>

class Identifier;
struct IRState;
class TryCatchStatement;

namespace llvm {
class AllocaInst;
class BasicBlock;
class GlobalVariable;
class MDNode;
class Value;
}

/// Represents a position on the stack of currently active cleanup scopes.
///
/// Since we always need to run a contiguous part of the stack (or all) in
/// order, two cursors (one of which is usually the currently top of the stack)
/// are enough to identify a sequence of cleanups to run.
using CleanupCursor = size_t;

////////////////////////////////////////////////////////////////////////////////

class TryCatchFinallyScopes;

/// Represents a scope for a TryCatchStatement.
class TryCatchScope {
public:
  /// Stores information to be able to branch to a catch clause if it matches.
  ///
  /// Each catch body is emitted only once, but may be target from many landing
  /// pads (in case of nested catch or cleanup scopes).
  struct CatchBlock {
    /// The ClassInfo reference corresponding to the type to match the
    /// exception object against.
    llvm::GlobalVariable *classInfoPtr;
    /// The block to branch to if the exception type matches.
    llvm::BasicBlock *bodyBB;
    // PGO branch weights for the exception type match branch.
    // (first weight is for match, second is for mismatch)
    llvm::MDNode *branchWeights;
  };

  /// The catch bodies are emitted when constructing a TryCatchScope (before the
  /// specified `endbb` block, which should be the try continuation block).
  TryCatchScope(IRState &irs, llvm::Value *ehPtrSlot, TryCatchStatement *stmt,
                llvm::BasicBlock *endbb);

  CleanupCursor getCleanupScope() const { return cleanupScope; }
  bool isCatchingNonExceptions() const { return catchesNonExceptions; }

  /// Returns the list of catch blocks, needed for landing pad emission.
  const std::vector<CatchBlock> &getCatchBlocks() const;

private:
  TryCatchStatement *stmt;
  llvm::BasicBlock *endbb;
  CleanupCursor cleanupScope;
  bool catchesNonExceptions;

  std::vector<CatchBlock> catchBlocks;

  void emitCatchBodies(IRState &irs, llvm::Value *ehPtrSlot);
  void emitCatchBodiesMSVC(IRState &irs, llvm::Value *ehPtrSlot);
};

////////////////////////////////////////////////////////////////////////////////

/// Represents a scope (in abstract terms, not curly braces) that requires a
/// piece of cleanup code to be run whenever it is left, whether as part of
/// normal control flow or exception unwinding.
///
/// This includes finally blocks (which are also generated by the frontend for
/// running the destructors of non-temporary variables) and the destructors of
/// temporaries (which are unfortunately not lowered by the frontend).
///
/// Our goal is to only emit each cleanup once such as to avoid generating an
/// exponential number of basic blocks/landing pads for handling all the
/// different ways of exiting a deeply nested scope (consider e.g. ten
/// local variables with destructors, each of which might throw itself).
class CleanupScope {
public:
  CleanupScope(llvm::BasicBlock *beginBlock, llvm::BasicBlock *endBlock);

  llvm::BasicBlock *run(IRState &irs, llvm::BasicBlock *sourceBlock,
                        llvm::BasicBlock *continueWith);

  /// MSVC uses C++ exception handling that puts cleanup blocks into funclets.
  /// This means that we cannot use a branch selector and conditional branches
  /// at cleanup exit to continue with different targets.
  /// Instead we make a full copy of the cleanup code for every target.
  llvm::BasicBlock *runCopying(IRState &irs, llvm::BasicBlock *sourceBlock,
                               llvm::BasicBlock *continueWith,
                               llvm::BasicBlock *unwindTo = nullptr,
                               llvm::Value *funclet = nullptr);

  llvm::BasicBlock *beginBlock() const { return blocks.front(); }
  llvm::BasicBlock *endBlock() const { return blocks.back(); }

private:
  std::vector<llvm::BasicBlock *> blocks;

  /// The branch selector variable, or null if not created yet.
  llvm::AllocaInst *branchSelector = nullptr;

  /// Describes a particular way to leave a cleanup scope and continue execution
  /// with another one.
  ///
  /// In general, there can be multiple ones (normal exit, early returns,
  /// breaks/continues, exceptions, and so on).
  struct CleanupExitTarget {
    explicit CleanupExitTarget(llvm::BasicBlock *t) : branchTarget(t) {}

    /// The target basic block to branch to after running the cleanup.
    llvm::BasicBlock *branchTarget = nullptr;

    /// The basic blocks that want to continue with this target after running
    /// the cleanup. We need to keep this information around so we can insert
    /// stores to the branch selector variable when converting from one to two
    /// targets.
    std::vector<llvm::BasicBlock *> sourceBlocks;

    /// MSVC: The basic blocks that are executed when going this route
    std::vector<llvm::BasicBlock *> cleanupBlocks;
  };

  /// Stores all possible targets blocks after running this cleanup, along
  /// with what predecessors want to continue at that target. The index in
  /// the vector corresponds to the branch selector value for that target.
  // Note: This is of course a bad choice of data structure for many targets
  // complexity-wise. However, situations where this matters should be
  // exceedingly rare in both hand-written as well as generated code.
  std::vector<CleanupExitTarget> exitTargets;
};

////////////////////////////////////////////////////////////////////////////////

/// Keeps track of source and target label of a goto.
///
/// Used if we cannot immediately emit all the code for a jump because we have
/// not generated code for the target yet.
struct GotoJump {
  // The location of the goto instruction, for error reporting.
  Loc sourceLoc;

  /// The basic block which contains the goto as its terminator.
  llvm::BasicBlock *sourceBlock;

  /// While we have not found the actual branch target, we might need to
  /// create a "fake" basic block in order to be able to execute the cleanups
  /// (we do not keep branching information around after leaving the scope).
  llvm::BasicBlock *tentativeTarget;

  /// The label to target with the goto.
  Identifier *targetLabel;
};

////////////////////////////////////////////////////////////////////////////////

/// Manages both try/catch and cleanups (try/finally blocks, destructors)
/// stacks.
///
/// Note that the entire code generation process, and this class in particular,
/// depends heavily on the fact that we visit the statement/expression tree in
/// its natural order, i.e. depth-first and in lexical order. In other words,
/// the code here expects that after a cleanup/catch/etc. has been pushed,
/// the contents of the block are generated, and it is then popped again
/// afterwards. This is also encoded in the fact that none of the methods for
/// branching/running cleanups take a cursor for describing the "source" scope,
/// it is always assumed to be the current one.
class TryCatchFinallyScopes {
public:
  explicit TryCatchFinallyScopes(IRState &irs);
  ~TryCatchFinallyScopes();

  bool empty() const { return tryCatchScopes.empty() && cleanupScopes.empty(); }

  /// Registers a try/catch scope.
  /// The catch bodies are emitted just before registering the new scope.
  void pushTryCatch(TryCatchStatement *stmt, llvm::BasicBlock *endbb);

  /// Unregisters the last registered try/catch scope.
  void popTryCatch();

  /// Indicates whether there are any active catch blocks that handle
  /// non-Exception Throwables.
  bool isCatchingNonExceptions() const;

  /// Registers a piece of cleanup code to be run.
  ///
  /// The end block is expected not to contain a terminator yet. It will be
  /// added as needed, based on what follow-up blocks code from within this
  /// scope will branch to.
  void pushCleanup(llvm::BasicBlock *beginBlock, llvm::BasicBlock *endBlock);

  /// Terminates the current basic block with a branch to the cleanups needed
  /// for leaving the current scope and continuing execution at the target
  /// scope stack level.
  ///
  /// After running them, execution will branch to the given basic block.
  void runCleanups(CleanupCursor targetScope, llvm::BasicBlock *continueWith);

  /// Pops all the cleanups between the current scope and the target cursor.
  ///
  /// This does not insert any cleanup calls, use #runCleanups() beforehand.
  void popCleanups(CleanupCursor targetScope);

  /// Returns a cursor that identifies the current cleanup scope, to be later
  /// used with #runCleanups() et al.
  ///
  /// Note that this cursor is only valid as long as the current scope is not
  /// popped.
  CleanupCursor currentCleanupScope() const { return cleanupScopes.size(); }

  /// Registers a goto jump to a not yet visited label.
  ///
  /// TryCatchFinallyScopes needs to keep track of all existing cleanups which
  /// are popped before the goto target is resolved. These cleanups will be run
  /// at each goto site before jumping to the actual target.
  void registerUnresolvedGoto(Loc loc, Identifier *labelName);

  /// Resolves all unresolved gotos matching the specified label and makes sure
  /// they jump to the specified target block.
  void tryResolveGotos(Identifier *labelName, llvm::BasicBlock *targetBlock);

  /// Gets the landing pad for the current catches and cleanups.
  /// If there's no cached one, a new one will be emitted.
  llvm::BasicBlock *getLandingPad();

private:
  IRState &irs;
  llvm::AllocaInst *ehPtrSlot = nullptr;
  /// Similar story to ehPtrSlot, but for the selector value.
  llvm::AllocaInst *ehSelectorSlot = nullptr;
  llvm::BasicBlock *resumeUnwindBlock = nullptr;

  std::vector<TryCatchScope> tryCatchScopes;

  /// cleanupScopes[i] contains the information to go from
  /// currentCleanupScope() == i + 1 to currentCleanupScope() == i.
  std::vector<CleanupScope> cleanupScopes;

  /// Keeps track of all the gotos originating from somewhere inside a cleanup
  /// scope for which we have not found the label yet (because it occurs
  /// lexically later in the function).
  // Note: Should also be a dense map from source block to the rest of the
  // data if we expect many gotos.
  using Gotos = std::vector<GotoJump>;
  /// The first element represents the stack of unresolved top-level gotos
  /// (no cleanups).
  std::vector<Gotos> unresolvedGotosPerCleanupScope;

  /// Gets the unresolved gotos for the current cleanup scope.
  std::vector<GotoJump> &currentUnresolvedGotos();

  using LandingPads = std::vector<llvm::BasicBlock *>;
  /// Landing pads are cached via a dedicated stack for each cleanup scope (one
  /// element is pushed to/popped from the back on entering/leaving a try-catch
  /// block).
  /// The first element represents the stack of top-level landing pads (no
  /// cleanups).
  std::vector<LandingPads> landingPadsPerCleanupScope;

  llvm::BasicBlock *&getLandingPadRef(CleanupCursor scope);

  /// Emits a landing pad to honor all the active cleanups and catches.
  llvm::BasicBlock *emitLandingPad();

  /// Internal version that allows specifying the scope at which to start
  /// emitting the cleanups.
  void runCleanups(CleanupCursor sourceScope, CleanupCursor targetScope,
                   llvm::BasicBlock *continueWith);

  /// Returns the stack slot that contains the exception object pointer while a
  /// landing pad is active, lazily creating it as needed.
  ///
  /// This value must dominate all uses; first storing it, and then loading it
  /// when calling _d_eh_resume_unwind. If we take a select at the end of any
  /// cleanups on the way to the latter, the value must also dominate all other
  /// predecessors of the cleanup. Thus, we just use a single alloca in the
  /// entry BB of the function.
  llvm::AllocaInst *getOrCreateEhPtrSlot();

  /// Returns the basic block with the call to the unwind resume function.
  ///
  /// Because of ehPtrSlot, we do not need more than one, so might as well
  /// save on code size and reuse it.
  llvm::BasicBlock *getOrCreateResumeUnwindBlock();

  // MSVC
  llvm::BasicBlock *emitLandingPadMSVC(CleanupCursor cleanupScope);
  void runCleanupCopies(CleanupCursor sourceScope, CleanupCursor targetScope,
                        llvm::BasicBlock *continueWith);
  llvm::BasicBlock *runCleanupPad(CleanupCursor scope,
                                  llvm::BasicBlock *unwindTo);
};

#endif