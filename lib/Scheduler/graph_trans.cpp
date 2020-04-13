#include "opt-sched/Scheduler/graph_trans.h"
#include "opt-sched/Scheduler/bit_vector.h"
#include "opt-sched/Scheduler/logger.h"
#include "opt-sched/Scheduler/register.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <list>
#include <vector>

using namespace llvm::opt_sched;

// Gets the registers which may be lengthened by scheduling nodeB after nodeA,
// as compared to if nodeB was scheduled before nodeA
static llvm::SmallVector<const Register *, 10> possiblyLengthenedIfAfterOther(
    const SchedInstruction *nodeB, llvm::ArrayRef<const Register *> bUses,
    const SchedInstruction *nodeA, llvm::ArrayRef<const Register *> aUses) {
  llvm::SmallVector<const Register *, 10> result;

  llvm::copy_if(bUses, std::back_inserter(result), [&](const Register *useB) {
    // Is this register also used by A?
    // If so, reordering A and B would have no effect on this register's
    // live range.
    const bool usedByA =
        llvm::any_of(aUses, [&](const Register *useA) { return useA == useB; });
    // If this register isn't used by A, is it at least used
    // by some successor? If so, reordering A and B would have no effect on
    // this register's live range, as it must live until C.
    const auto usedByC = [&] {
      return llvm::any_of(
          useB->GetUseList(), [&](const SchedInstruction *user) {
            // Given: [... B ... A ...]
            // We need to prove that the register `useB` won't be used by an
            // instruction before A but after B. In the hypothetical schedule we
            // are considering, A currently appears after B. Thus, it is
            // sufficient to show that this register has a user C that is a
            // successor of A.
            //
            // This is more relaxed than showing that C is a successor of B, as
            // RcrsvScsr(B) is a subset of RcrsvScsr(A).
            return user != nodeB &&
                   nodeA->IsRcrsvScsr(const_cast<SchedInstruction *>(user));
          });
    };

    return !usedByA && !usedByC();
  });

  return result;
}

// Converts a class type and a function signature into a pointer to member
// function.
// Usage: memfn_t<Cls, int(double) const> ==> int (Cls::*)(double) const
template <typename Cls, typename FnSig> using memfn_t = FnSig Cls::*;

// Gets the Uses or Defs for the given SchedInstruction.
template <std::size_t N,
          memfn_t<SchedInstruction, int16_t(Register **&)> UsesOrDefsFn>
static llvm::SmallVector<const Register *, N> getSet(SchedInstruction *node) {
  Register **uses;
  const int useCount = (node->*UsesOrDefsFn)(uses);
  // Call the (Iter, Iter) constructor.
  return {uses, uses + useCount};
}

// Calls llvm::zip_first(...) over the ranges, but wraps each range in an
// llvm::ArrayRef.
template <typename... Rs>
static auto zipFirstRefs(const Rs &... ranges)
    -> decltype(llvm::zip_first(llvm::makeArrayRef(ranges)...)) {
  return llvm::zip_first(llvm::makeArrayRef(ranges)...);
}

GraphTrans::GraphTrans(DataDepGraph *dataDepGraph) {
  assert(dataDepGraph != NULL);

  SetDataDepGraph(dataDepGraph);
  SetNumNodesInGraph(dataDepGraph->GetInstCnt());
}

bool GraphTrans::AreNodesIndep_(SchedInstruction *inst1,
                                SchedInstruction *inst2) {
  // The nodes are independent if there is no path from srcInst to dstInst.
  if (inst1 != inst2 && !inst1->IsRcrsvPrdcsr(inst2) &&
      !inst1->IsRcrsvScsr(inst2)) {
#ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info("Nodes %d and %d are independent", inst1->GetNum(),
                 inst2->GetNum());
#endif
    return true;
  } else
    return false;
}

void GraphTrans::UpdatePrdcsrAndScsr_(SchedInstruction *nodeA,
                                      SchedInstruction *nodeB) {
  LinkedList<GraphNode> *nodeBScsrLst = nodeB->GetRcrsvNghbrLst(DIR_FRWRD);
  LinkedList<GraphNode> *nodeAPrdcsrLst = nodeA->GetRcrsvNghbrLst(DIR_BKWRD);

  // Update lists for the nodes themselves.
  nodeA->AddRcrsvScsr(nodeB);
  nodeB->AddRcrsvPrdcsr(nodeA);

  for (GraphNode *X = nodeAPrdcsrLst->GetFrstElmnt(); X != NULL;
       X = nodeAPrdcsrLst->GetNxtElmnt()) {

    for (GraphNode *Y = nodeBScsrLst->GetFrstElmnt(); Y != NULL;
         Y = nodeBScsrLst->GetNxtElmnt()) {
      // Check if Y is reachable f
      if (!X->IsRcrsvScsr(Y)) {
        Y->AddRcrsvPrdcsr(X);
        X->AddRcrsvScsr(Y);
      }
    }
  }
}

StaticNodeSupTrans::StaticNodeSupTrans(DataDepGraph *dataDepGraph,
                                       bool IsMultiPass_)
    : GraphTrans(dataDepGraph) {
  IsMultiPass = IsMultiPass_;
}

bool StaticNodeSupTrans::TryAddingSuperiorEdge_(SchedInstruction *nodeA,
                                                SchedInstruction *nodeB) {
  // Return this flag which designates whether an edge was added.
  bool edgeWasAdded = false;

  if (nodeA->GetNodeID() > nodeB->GetNodeID())
    std::swap(nodeA, nodeB);

  if (NodeIsSuperior_(nodeA, nodeB)) {
    AddSuperiorEdge_(nodeA, nodeB);
    edgeWasAdded = true;
  } else if (NodeIsSuperior_(nodeB, nodeA)) {
    AddSuperiorEdge_(nodeB, nodeA);
    // Swap nodeIDs
    // int tmp = nodeA->GetNodeID();
    // nodeA->SetNodeID(nodeB->GetNodeID());
    // nodeB->SetNodeID(tmp);
    edgeWasAdded = true;
  }

  return edgeWasAdded;
}

void StaticNodeSupTrans::AddSuperiorEdge_(SchedInstruction *nodeA,
                                          SchedInstruction *nodeB) {
#if defined(IS_DEBUG_GRAPH_TRANS_RES) || defined(IS_DEBUG_GRAPH_TRANS)
  Logger::Info("Node %d is superior to node %d", nodeA->GetNum(),
               nodeB->GetNum());
#endif
  GetDataDepGraph_()->CreateEdge(nodeA, nodeB, 0, DEP_OTHER);
  UpdatePrdcsrAndScsr_(nodeA, nodeB);
}

FUNC_RESULT StaticNodeSupTrans::ApplyTrans() {
  InstCount numNodes = GetNumNodesInGraph_();
  DataDepGraph *graph = GetDataDepGraph_();
  // A list of independent nodes.
  std::list<std::pair<SchedInstruction *, SchedInstruction *>> indepNodes;
  bool didAddEdge = false;
#ifdef IS_DEBUG_GRAPH_TRANS
  Logger::Info("Applying node superiority graph transformation.");
#endif

  // For the first pass visit all nodes. Add sets of independent nodes to a
  // list.
  for (int i = 0; i < numNodes; i++) {
    SchedInstruction *nodeA = graph->GetInstByIndx(i);
    for (int j = i + 1; j < numNodes; j++) {
      if (i == j)
        continue;
      SchedInstruction *nodeB = graph->GetInstByIndx(j);

#ifdef IS_DEBUG_GRAPH_TRANS
      Logger::Info("Checking nodes %d:%d", i, j);
#endif
      if (AreNodesIndep_(nodeA, nodeB)) {
        didAddEdge = TryAddingSuperiorEdge_(nodeA, nodeB);
        // If the nodes are independent and no superiority was found add the
        // nodes to a list for
        // future passes.
        if (!didAddEdge)
          indepNodes.push_back(std::make_pair(nodeA, nodeB));
      }
    }
  }

  if (IsMultiPass)
    nodeMultiPass_(indepNodes);

  return RES_SUCCESS;
}

bool StaticNodeSupTrans::NodeIsSuperior_(SchedInstruction *nodeA,
                                         SchedInstruction *nodeB) {
  DataDepGraph *graph = GetDataDepGraph_();

  if (nodeA->GetIssueType() != nodeB->GetIssueType()) {
#ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info("Node %d is not of the same issue type as node %d",
                 nodeA->GetNum(), nodeB->GetNum());
#endif
    return false;
  }

  // The predecessor list of A must be a sub-list of the predecessor list of B.
  BitVector *predsA = nodeA->GetRcrsvNghbrBitVector(DIR_BKWRD);
  BitVector *predsB = nodeB->GetRcrsvNghbrBitVector(DIR_BKWRD);
  if (!predsA->IsSubVector(predsB)) {
#ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info(
        "Pred list of node %d is not a sub-list of the pred list of node %d",
        nodeA->GetNum(), nodeB->GetNum());
#endif
    return false;
  }

  // The successor list of B must be a sub-list of the successor list of A.
  BitVector *succsA = nodeA->GetRcrsvNghbrBitVector(DIR_FRWRD);
  BitVector *succsB = nodeB->GetRcrsvNghbrBitVector(DIR_FRWRD);
  if (!succsB->IsSubVector(succsA)) {
#ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info(
        "Succ list of node %d is not a sub-list of the succ list of node %d",
        nodeB->GetNum(), nodeA->GetNum());
#endif
    return false;
  }

  // For every virtual register that belongs to the Use set of B but does not
  // belong to the Use set of A
  // there must be at least one instruction C that is distint from A nad B and
  // belongs to the
  // recurisve sucessor lits of both A and B.
  //
  // For every vitrual register that would have its live range lengthened by
  // scheduling B after A,
  // there must be a register of the same time that would have its live range
  // shortened by scheduling
  // A before B.

  // First find registers that belong to the Use Set of B but not to the Use Set
  // of A.
  // TODO (austin) modify wrapper code so it is easier to identify physical
  // registers.
  const int regTypes = graph->GetRegTypeCnt();

  const llvm::SmallVector<const Register *, 10> usesA =
      ::getSet<10, &SchedInstruction::GetUses>(nodeA);
  const llvm::SmallVector<const Register *, 10> usesB =
      ::getSet<10, &SchedInstruction::GetUses>(nodeB);

  const llvm::SmallVector<const Register *, 10> defsA =
      ::getSet<10, &SchedInstruction::GetDefs>(nodeA);
  const llvm::SmallVector<const Register *, 10> defsB =
      ::getSet<10, &SchedInstruction::GetDefs>(nodeB);

  // The number of registers that will be lengthened by
  // scheduling B after A. Indexed by register type.
  llvm::SmallVector<int, 10> usesLengthenedByB(regTypes);
  llvm::SmallVector<int, 10> usesShortenedByA(regTypes);

  // Optimality condition: R in Use(B) - Use(A), but there is no C successor
  // which uses R.
  const auto usesLengthenedByBUnclassified =
      possiblyLengthenedIfAfterOther(nodeB, usesB, nodeA, usesA);
  for (const Register *bLastUse : usesLengthenedByBUnclassified) {
    ++usesLengthenedByB[bLastUse->GetType()];
  }

  // Repeat for A, to find registers shortened by moving A earlier.
  const auto usesLengthenedByAUnclassified =
      possiblyLengthenedIfAfterOther(nodeA, usesA, nodeB, usesB);
  for (const Register *aLastUse : usesLengthenedByAUnclassified) {
    ++usesShortenedByA[aLastUse->GetType()];
  }

  // For each register type, the number of registers whose live range was
  // shortened by scheduling A earlier must be >= than the number of registers
  // whose live range was lengthened by scheduling B earlier
  const bool swapShortensUses =
      llvm::all_of(::zipFirstRefs(usesShortenedByA, usesLengthenedByB),
                   [](std::tuple<int, int> regUses) {
                     return std::get<0>(regUses) >= std::get<1>(regUses);
                   });

  if (!swapShortensUses) {
#ifdef IS_DEBUG_GRAPH_TRANS
    Logger::Info("Live range condition 1 failed");
#endif
    return false;
  }

  // For each register type, the number of registers defined by A is less than
  // or equal to the number of registers defined by B.
  llvm::SmallVector<int, 10> numADefsByType(regTypes);
  llvm::SmallVector<int, 10> numBDefsByType(regTypes);

  for (const Register *reg : defsA)
    ++numADefsByType[reg->GetType()];

  for (const Register *reg : defsB)
    ++numBDefsByType[reg->GetType()];

  return llvm::all_of(::zipFirstRefs(numADefsByType, numBDefsByType),
                      [](std::tuple<int, int> regDefs) {
                        return std::get<0>(regDefs) <= std::get<1>(regDefs);
                      });
}

void StaticNodeSupTrans::nodeMultiPass_(
    std::list<std::pair<SchedInstruction *, SchedInstruction *>> indepNodes) {
#ifdef IS_DEBUG_GRAPH_TRANS
  Logger::Info("Applying multi-pass node superiority");
#endif
  // Try to add superior edges until there are no more independent nodes or no
  // edges can be added.
  bool didAddEdge = true;
  while (didAddEdge && indepNodes.size() > 0) {
    std::list<std::pair<SchedInstruction *, SchedInstruction *>>::iterator
        pair = indepNodes.begin();
    didAddEdge = false;

    while (pair != indepNodes.end()) {
      SchedInstruction *nodeA = (*pair).first;
      SchedInstruction *nodeB = (*pair).second;

      if (!AreNodesIndep_(nodeA, nodeB)) {
        pair = indepNodes.erase(pair);
      } else {
        bool result = TryAddingSuperiorEdge_(nodeA, nodeB);
        // If a superior edge was added remove the pair of nodes from the list.
        if (result) {
          pair = indepNodes.erase(pair);
          didAddEdge = true;
        } else
          pair++;
      }
    }
  }
}
