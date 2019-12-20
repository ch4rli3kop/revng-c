#pragma once

//
// Copyright (c) rev.ng Srls. See LICENSE.md for details.
//

#include <compare>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"

#include "revng/ADT/FilteredGraphTraits.h"
#include "revng/Support/Assert.h"

namespace llvm {

class SCEV;

} // end namespace llvm

namespace dla {

/// A representation of a pointer to a type.
class LayoutTypePtr {
  const llvm::Value *V;
  unsigned FieldIdx;

public:
  explicit LayoutTypePtr(const llvm::Value *Val,
                         unsigned Idx = std::numeric_limits<unsigned>::max()) :
    V(Val), FieldIdx(Idx) {
    revng_assert(Val != nullptr);
    using llvm::cast;
    using llvm::dyn_cast;
    using llvm::isa;
    [[maybe_unused]] const llvm::Type *Ty = V->getType();
    // We only accept Functions or Values with integer or pointer type.
    revng_assert(isa<llvm::Function>(V) or isa<llvm::IntegerType>(Ty)
                 or isa<llvm::PointerType>(Ty));

    // FieldIdx != std::numeric_limits<unsigned>::max() if and only if V is a
    // Function that returns a struct.
    const auto *F = dyn_cast<llvm::Function>(V);
    const auto *StructTy = (F == nullptr) ?
                             nullptr :
                             dyn_cast<llvm::StructType>(F->getReturnType());
    [[maybe_unused]] bool VIsFunctionAndReturnsStruct = StructTy != nullptr;
    revng_assert(VIsFunctionAndReturnsStruct
                 xor (FieldIdx == std::numeric_limits<unsigned>::max()));

    // If V is a Function that returns a struct then FieldIdx < number of
    // elements of the returned struct.
    revng_assert(not VIsFunctionAndReturnsStruct
                 or FieldIdx < StructTy->getNumElements());
  }

  LayoutTypePtr() = delete;
  ~LayoutTypePtr() = default;
  LayoutTypePtr(const LayoutTypePtr &) = default;
  LayoutTypePtr(LayoutTypePtr &&) = default;
  LayoutTypePtr &operator=(const LayoutTypePtr &) = default;
  LayoutTypePtr &operator=(LayoutTypePtr &&) = default;

  std::strong_ordering operator<=>(const LayoutTypePtr &Other) const {
    if (auto Cmp = V <=> Other.V; Cmp != 0)
      return Cmp;
    return FieldIdx <=> Other.FieldIdx;
  }

  bool operator<(const LayoutTypePtr &Other) const {
    return (*this <=> Other) < 0;
  }

  bool operator==(const LayoutTypePtr &Other) const {
    return operator<=>(Other) == 0;
  }

  void print(llvm::raw_ostream &Out) const;
  friend struct std::less<dla::LayoutTypePtr>;
}; // end class LayoutTypePtr

/// Class used to mark InstanceLinkTags between LayoutTypes
struct OffsetExpression {
  llvm::SmallVector<std::optional<int64_t>, 4> TripCounts;
  llvm::SmallVector<int64_t, 4> Strides;
  int64_t Offset;

  explicit OffsetExpression(int64_t Off) :
    TripCounts(), Strides(), Offset(Off) {}
  explicit OffsetExpression() : OffsetExpression(0LL){};

  std::strong_ordering operator<=>(const OffsetExpression &Other) const {
    auto OffsetCompare = Offset <=> Other.Offset;
    if (OffsetCompare != 0)
      return OffsetCompare;

    if (Strides < Other.Strides)
      return std::strong_ordering::less;
    else if (Other.Strides < Strides)
      return std::strong_ordering::greater;

    if (TripCounts < Other.TripCounts)
      return std::strong_ordering::less;
    else if (Other.TripCounts < TripCounts)
      return std::strong_ordering::greater;

    return std::strong_ordering::equal;
  }

  bool operator<(const OffsetExpression &Other) const {
    return (*this <=> Other) < 0;
  }
}; // end class OffsetExpression

class TypeLinkTag {
public:
  enum LinkKind {
    LK_Inheritance,
    LK_Equality,
    LK_Instance,
    LK_All,
  };

  static const char *toString(enum LinkKind K) {
    switch (K) {
    case LK_Inheritance:
      return "Inheritance";
    case LK_Equality:
      return "Equality";
    case LK_Instance:
      return "Instance";
    case LK_All:
      return "None";
    }
    revng_unreachable();
  }

protected:
  OffsetExpression OE;
  const LinkKind Kind;

  explicit TypeLinkTag(LinkKind K, OffsetExpression &&O) : OE(O), Kind(K) {}

  // TODO: potentially we are interested in marking TypeLinkTags with some info
  // that allows us to track which step on the type system has created them.
  // However, this is not necessary now, so I'll leave it for when we have
  // identified more clearly if we really need it and why.

public:
  TypeLinkTag() = delete;

  LinkKind getKind() const { return Kind; }

  const OffsetExpression &getOffsetExpr() const {
    revng_assert(getKind() == LK_Instance);
    return OE;
  }

  static TypeLinkTag equalityTag() {
    return TypeLinkTag(LK_Equality, OffsetExpression{});
  }

  static TypeLinkTag inheritanceTag() {
    return TypeLinkTag(LK_Inheritance, OffsetExpression{});
  }

  // This method is templated just to enable perfect forwarding.
  template<typename OffsetExpressionT>
  static TypeLinkTag instanceTag(OffsetExpressionT &&O) {
    return TypeLinkTag(LK_Instance, std::forward<OffsetExpressionT>(O));
  }

  std::strong_ordering operator<=>(const TypeLinkTag &Other) const {
    if (auto Cmp = (Kind <=> Other.Kind); Cmp != 0)
      return Cmp;
    return OE <=> Other.OE;
  }

  bool operator<(const TypeLinkTag &Other) const {
    return (*this <=> Other) < 0;
  }
}; // end class TypeLinkTag

struct LayoutType {
  // TODO: do we really need the accesses?
  llvm::SmallPtrSet<const llvm::Use *, 1> Accesses{};
  uint64_t Size{};
}; // end class LayoutType

class LayoutTypeSystem;

struct LayoutTypeSystemNode {
  const uint64_t ID = 0ULL;
  using Link = std::pair<LayoutTypeSystemNode *, const TypeLinkTag *>;
  using NeighborsSet = std::set<Link>;
  NeighborsSet Successors{};
  NeighborsSet Predecessors{};
  LayoutType L{};
  LayoutTypeSystemNode(uint64_t I) : ID(I) {}

public:
  // This method should never be called, but it's necessary to be able to use
  // some llvm::GraphTraits algorithms, otherwise they wouldn't compile.
  LayoutTypeSystem *getParent() {
    revng_unreachable();
    return nullptr;
  }

  void printAsOperand(llvm::raw_ostream &OS, bool /* unused */);
};

inline bool hasValidLayout(const LayoutTypeSystemNode *N) {
  if (N == nullptr)
    return false;
  return not N->L.Accesses.empty();
}

struct LayoutTypeSystemNodePtrCompare {
  using is_transparent = std::true_type;

private:
  struct Helper {
    const LayoutTypeSystemNode *P;
    Helper() = default;
    ~Helper() = default;
    Helper(const Helper &) = default;
    Helper(Helper &&) = default;
    Helper &operator=(const Helper &) = default;
    Helper &operator=(Helper &&) = default;
    Helper(const LayoutTypeSystemNode *Ptr) : P(Ptr) {}
    Helper(const std::unique_ptr<LayoutTypeSystemNode> &Ptr) : P(Ptr.get()) {}
  };

public:
  bool operator()(const Helper A, const Helper B) const { return A.P < B.P; }
};

class LayoutTypeSystem {
public:
  using Node = LayoutTypeSystemNode;
  using NodePtr = LayoutTypeSystemNode *;
  using NodeUniquePtr = std::unique_ptr<LayoutTypeSystemNode>;

  static dla::LayoutTypeSystem::NodePtr
  getNodePtr(const dla::LayoutTypeSystem::NodeUniquePtr &P) {
    return P.get();
  }

  LayoutTypeSystem(llvm::Module &Mod) : M(Mod) {}

  llvm::Module &getModule() const { return M; }

public:
  LayoutTypeSystemNode *getLayoutType(const llvm::Value *V, unsigned Id);

  LayoutTypeSystemNode *getLayoutType(const llvm::Value *V) {
    return getLayoutType(V, std::numeric_limits<unsigned>::max());
  };

  std::pair<LayoutTypeSystemNode *, bool>
  getOrCreateLayoutType(const llvm::Value *V, unsigned Id);

  std::pair<LayoutTypeSystemNode *, bool>
  getOrCreateLayoutType(const llvm::Value *V) {
    return getOrCreateLayoutType(V, std::numeric_limits<unsigned>::max());
  }

  llvm::SmallVector<LayoutTypeSystemNode *, 2>
  getLayoutTypes(const llvm::Value &V);

  llvm::SmallVector<std::pair<LayoutTypeSystemNode *, bool>, 2>
  getOrCreateLayoutTypes(const llvm::Value &V);

  LayoutTypeSystemNode *getLayoutType(const llvm::SCEV *S);

protected:
  // This method is templated only to enable perfect forwarding.
  template<typename TagT>
  std::pair<const TypeLinkTag *, bool>
  addLink(LayoutTypeSystemNode *Src, LayoutTypeSystemNode *Tgt, TagT &&Tag) {
    if (Src == nullptr or Tgt == nullptr or Src == Tgt)
      return std::make_pair(nullptr, false);
    revng_assert(Layouts.count(Src));
    revng_assert(Layouts.count(Tgt));
    auto It = LinkTags.insert(std::forward<TagT>(Tag)).first;
    revng_assert(It != LinkTags.end());
    const TypeLinkTag *T = &*It;
    bool New = Src->Successors.insert(std::make_pair(Tgt, T)).second;
    New |= Tgt->Predecessors.insert(std::make_pair(Src, T)).second;
    return std::make_pair(T, New);
  }

public:
  std::pair<const TypeLinkTag *, bool>
  addEqualityLink(LayoutTypeSystemNode *Src, LayoutTypeSystemNode *Tgt) {
    auto ForwardLinkTag = addLink(Src, Tgt, dla::TypeLinkTag::equalityTag());
    auto BackwardLinkTag = addLink(Tgt, Src, dla::TypeLinkTag::equalityTag());
    revng_assert(ForwardLinkTag == BackwardLinkTag);
    return ForwardLinkTag;
  }

  std::pair<const TypeLinkTag *, bool>
  addInheritanceLink(LayoutTypeSystemNode *Src, LayoutTypeSystemNode *Tgt) {
    return addLink(Src, Tgt, dla::TypeLinkTag::inheritanceTag());
  }

  // This method is templated just to enable perfect forwarding.
  template<typename OffsetExpressionT>
  std::pair<const TypeLinkTag *, bool>
  addInstanceLink(LayoutTypeSystemNode *Src,
                  LayoutTypeSystemNode *Tgt,
                  OffsetExpressionT &&OE) {
    using OET = OffsetExpressionT;
    return addLink(Src,
                   Tgt,
                   dla::TypeLinkTag::instanceTag(std::forward<OET>(OE)));
  }

  void dumpDotOnFile(const char *FName) const;

  void dumpDotOnFile(const std::string &FName) const {
    dumpDotOnFile(FName.c_str());
  }

  auto getNumLayouts() const { return Layouts.size(); }

  auto getLayoutsRange() const {
    return llvm::make_range(llvm::map_iterator(Layouts.begin(), getNodePtr),
                            llvm::map_iterator(Layouts.end(), getNodePtr));
  }

protected:
  void mergeNodes(LayoutTypeSystemNode *From,
                  LayoutTypeSystemNode *Into,
                  llvm::SmallSet<LayoutTypePtr, 2> *IntoTypePtrs);

public:
  void mergeNodes(LayoutTypeSystemNode *From, LayoutTypeSystemNode *Into) {
    return mergeNodes(From, Into, nullptr);
  }

  void mergeNodes(const std::vector<LayoutTypeSystemNode *> &ToMerge);

  const llvm::SmallSet<LayoutTypePtr, 2> &
  getLayoutTypePtrs(const LayoutTypeSystemNode *N) const {
    return LayoutToTypePtrsMap.at(N);
  }

  bool hasLayoutTypePtrs(const LayoutTypeSystemNode *N) const {
    return LayoutToTypePtrsMap.count(N);
  }

  void removeNode(LayoutTypeSystemNode *N);

private:
  // A reference to the associated Module
  llvm::Module &M;

  uint64_t NID = 0ULL;

  // Holds all the LayoutTypeSystemNode
  std::set<std::unique_ptr<LayoutTypeSystemNode>,
           LayoutTypeSystemNodePtrCompare>
    Layouts;

  // Maps llvm::Value to layout types.
  // This map is updated along the way when the DLA algorithm merges
  // LayoutTypeSystemNodes that are considered to represent the same type.
  std::map<LayoutTypePtr, LayoutTypeSystemNode *> TypePtrToLayoutMap;

  // Maps layout types to the set of LayoutTypePtr representing the llvm::Value
  // that generated them.
  std::map<const LayoutTypeSystemNode *, llvm::SmallSet<LayoutTypePtr, 2>>
    LayoutToTypePtrsMap;

  // Holds the link tags, so that they can be deduplicated and referred to using
  // TypeLinkTag * in the links inside LayoutTypeSystemNode
  std::set<TypeLinkTag> LinkTags;

public:
  // Checks that is valid, and returns true if it is, false otherwise
  bool verifyConsistency() const;
  // Checks that is valid and a DAG, and returns true if it is, false otherwise
  bool verifyDAG() const;
  // Checks that is valid and a DAG, and returns true if it is, false otherwise
  bool verifyInheritanceDAG() const;
  // Checks that is valid and a DAG, and returns true if it is, false otherwise
  bool verifyInstanceDAG() const;
  // Checks that the type system, filtered looking only at inheritance edges, is
  // a tree, meaning that a give LayoutTypeSystemNode cannot inherit from two
  // different LayoutTypeSystemNodes.
  bool verifyInheritanceTree() const;
  // Checks that there are no leaf nodes without valid layout information
  bool verifyLeafs() const;
  // Checks that there are no equality edges.
  bool verifyNoEquality() const;
}; // end class LayoutTypeSystem

} // end namespace dla

template<>
struct llvm::GraphTraits<dla::LayoutTypeSystemNode *> {
protected:
  using NodeT = dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->Successors.begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->Successors.end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->Successors.begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->Successors.end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<const dla::LayoutTypeSystemNode *> {
protected:
  using NodeT = const dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->Successors.begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->Successors.end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->Successors.begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->Successors.end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<llvm::Inverse<dla::LayoutTypeSystemNode *>> {
protected:
  using NodeT = dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->Predecessors.begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->Predecessors.end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->Predecessors.begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->Predecessors.end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<llvm::Inverse<const dla::LayoutTypeSystemNode *>> {
protected:
  using NodeT = const dla::LayoutTypeSystemNode;

public:
  using NodeRef = NodeT *;
  using EdgeRef = const NodeT::NeighborsSet::value_type;

  static NodeRef edge_dest(EdgeRef E) { return E.first; }
  using EdgeDestT = NodeRef (*)(EdgeRef);

  using ChildEdgeIteratorType = NodeT::NeighborsSet::iterator;
  using ChildIteratorType = llvm::mapped_iterator<ChildEdgeIteratorType,
                                                  EdgeDestT>;

  static NodeRef getEntryNode(const NodeRef &N) { return N; }

  static ChildIteratorType child_begin(NodeRef N) {
    return llvm::map_iterator(N->Predecessors.begin(), edge_dest);
  }
  static ChildIteratorType child_end(NodeRef N) {
    return llvm::map_iterator(N->Predecessors.end(), edge_dest);
  }

  static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
    return N->Predecessors.begin();
  }
  static ChildEdgeIteratorType child_edge_end(NodeRef N) {
    return N->Predecessors.end();
  }
}; // end struct llvm::GraphTraits<dla::LayoutTypeSystemNode *>

template<>
struct llvm::GraphTraits<const dla::LayoutTypeSystem *>
  : public llvm::GraphTraits<const dla::LayoutTypeSystemNode *> {
protected:
  using NodeSetItT = std::set<dla::LayoutTypeSystem::NodeUniquePtr>::iterator;
  using NodeUniquePtr = dla::LayoutTypeSystem::NodeUniquePtr;
  using GetPtrT = dla::LayoutTypeSystem::NodePtr (*)(const NodeUniquePtr &);

public:
  using nodes_iterator = llvm::mapped_iterator<NodeSetItT, GetPtrT>;

  static NodeRef getEntryNode(const dla::LayoutTypeSystem *) { return nullptr; }

  static nodes_iterator nodes_begin(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().begin();
  }

  static nodes_iterator nodes_end(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().end();
  }

  static unsigned size(const dla::LayoutTypeSystem *G) {
    return G->getNumLayouts();
  }
}; // struct llvm::GraphTraits<dla::LayoutTypeSystem>

template<>
struct llvm::GraphTraits<dla::LayoutTypeSystem *>
  : public llvm::GraphTraits<dla::LayoutTypeSystemNode *> {
protected:
  using NodeSetItT = std::set<dla::LayoutTypeSystem::NodeUniquePtr>::iterator;
  using NodeUniquePtr = dla::LayoutTypeSystem::NodeUniquePtr;
  using GetPtrT = dla::LayoutTypeSystem::NodePtr (*)(const NodeUniquePtr &);

public:
  using nodes_iterator = llvm::mapped_iterator<NodeSetItT, GetPtrT>;

  static NodeRef getEntryNode(const dla::LayoutTypeSystem *) { return nullptr; }

  static nodes_iterator nodes_begin(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().begin();
  }

  static nodes_iterator nodes_end(const dla::LayoutTypeSystem *G) {
    return G->getLayoutsRange().end();
  }

  static unsigned size(dla::LayoutTypeSystem *G) { return G->getNumLayouts(); }
}; // struct llvm::GraphTraits<dla::LayoutTypeSystem>

namespace dla {

template<dla::TypeLinkTag::LinkKind K>
inline bool hasLinkKind(const dla::LayoutTypeSystemNode::Link &L) {
  if constexpr (K == dla::TypeLinkTag::LinkKind::LK_All)
    return true;
  else
    return L.second->getKind() == K;
}

inline bool
isEqualityEdge(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return hasLinkKind<TypeLinkTag::LinkKind::LK_Equality>(E);
}

inline bool
isInheritanceEdge(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return hasLinkKind<TypeLinkTag::LinkKind::LK_Inheritance>(E);
}

inline bool
isInstanceEdge(const llvm::GraphTraits<LayoutTypeSystemNode *>::EdgeRef &E) {
  return hasLinkKind<TypeLinkTag::LinkKind::LK_Instance>(E);
}

template<dla::TypeLinkTag::LinkKind K = dla::TypeLinkTag::LinkKind::LK_All>
inline bool isLeaf(const LayoutTypeSystemNode *N) {
  using LTSN = const LayoutTypeSystemNode;
  using GraphNodeT = LTSN *;
  using FilteredNodeT = EdgeFilteredGraph<GraphNodeT, hasLinkKind<K>>;
  using GT = llvm::GraphTraits<FilteredNodeT>;
  return GT::child_begin(N) == GT::child_end(N);
}

inline bool isInheritanceLeaf(const LayoutTypeSystemNode *N) {
  return isLeaf<dla::TypeLinkTag::LinkKind::LK_Inheritance>(N);
}

inline bool isInstanceLeaf(const LayoutTypeSystemNode *N) {
  return isLeaf<dla::TypeLinkTag::LinkKind::LK_Instance>(N);
}

template<dla::TypeLinkTag::LinkKind K = dla::TypeLinkTag::LinkKind::LK_All>
inline bool isRoot(const LayoutTypeSystemNode *N) {
  using LTSN = const LayoutTypeSystemNode;
  using GraphNodeT = LTSN *;
  using FilteredNodeT = EdgeFilteredGraph<GraphNodeT, hasLinkKind<K>>;
  using IGT = llvm::GraphTraits<llvm::Inverse<FilteredNodeT>>;
  return IGT::child_begin(N) == IGT::child_end(N);
}

inline bool isInheritanceRoot(const LayoutTypeSystemNode *N) {
  return isRoot<dla::TypeLinkTag::LinkKind::LK_Inheritance>(N);
}

inline bool isInstanceRoot(const LayoutTypeSystemNode *N) {
  return isRoot<dla::TypeLinkTag::LinkKind::LK_Instance>(N);
}
} // end namespace dla

std::string dumpToString(const dla::OffsetExpression &OE);
std::string dumpToString(const dla::LayoutTypeSystemNode *N);