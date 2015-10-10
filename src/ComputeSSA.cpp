/*
 * Copyright (C) 2015 David Devecsery
 */
// #define SPECSFS_DEBUG

#include <algorithm>
#include <set>
#include <utility>
#include <vector>

#include "include/SpecSFS.h"

#include "include/Debug.h"

#include "include/SEG.h"
#include "include/ControlFlowGraph.h"


// Ramalingams {{{
// Transforms {{{
// Combine all SCCs in Xp in G
static void T4(CFG::ControlFlowGraph &G, const CFG::ControlFlowGraph &Xp) {
  // For each SCC in Xp combine the nodes in G
  dout("Running T4\n");
  // Do this by iterating G
  std::for_each(std::begin(G), std::end(G),
      [&G, &Xp](const CFG::ControlFlowGraph::node_iter_type &pnode) {
    // Get the node in G
    auto &nd = llvm::cast<CFG::Node>(*pnode);

    // Now, get the rep from Xp
    auto pxp_rep = Xp.tryGetNode<CFG::Node>(nd.id());
    // auto &xp_rep = Xp.getNode<CFG::Node>(nd.id());

    if (pxp_rep != nullptr) {
      auto &xp_rep = *pxp_rep;
      // If the rep from xp has a different ID than G:
      if (xp_rep.id() != nd.id()) {
        // Get the G node that should be rep, and unite the two
        auto &g_rep = G.getNode(xp_rep.id());

        // Remove the edge from rep to nd, as we're about to unite them
        G.removePred(nd.id(), xp_rep.id());

        g_rep.unite(G, nd);
      }
    }
  });

  dout("Finished T4\n");
}

// Now, for each P-node in G1 (output from T4) with precisely 1 predecessor,
//   we combine that node with its predecessor
static void T2(CFG::ControlFlowGraph &G, CFG::ControlFlowGraph &Xp) {
  // Visit Xp in topological order
  dout("Running T2\n");
  std::for_each(Xp.topo_begin(), Xp.topo_end(),
      [&G, &Xp](CFG::NodeID xp_id) {
    // llvm::dbgs() << "visiting node: " << xp_id << "\n";
    auto &xp_node = Xp.getNode(xp_id);
    auto &w_node = G.getNode<CFG::Node>(xp_node.id());
    // llvm::dbgs() << "preds().size() is: " << w_node.preds().size() << "\n";
    // NOTE: because we cleaned the edges, we can rely on w_node.preds().size()
    //   since we've removed duplicates from our edge set
    if (w_node.preds().size() == 1) {
      auto &pred_id = *std::begin(w_node.preds());
      auto &pred_node = G.getNode<CFG::Node>(pred_id);

      // We don't unite if we're our own predecessor
      if (w_node != pred_node) {
        // Remove the edge from pred to w before we unite
        G.removePred(pred_node.id(), w_node.id());
        pred_node.unite(G, w_node);
      }
    }
  });

  dout("Finished T2\n");
}

void T7(CFG::ControlFlowGraph &G) {
  dout("Running T7\n");
  std::for_each(std::begin(G), std::end(G),
      [&G](CFG::ControlFlowGraph::node_iter_type &pnode) {
      if (pnode != nullptr) {
        auto &node = llvm::cast<CFG::Node>(*pnode);

        // If its a c-node, remove all preceding edges
        if (node.c()) {
          // Note, we're copying node.preds here... so we can delete it
          std::vector<CFG::NodeID> preds(std::begin(node.preds()),
            std::end(node.preds()));

          std::for_each(std::begin(preds), std::end(preds),
              [&G, &node](CFG::NodeID pred_id) {
            G.removePred(node.id(), pred_id);
          });
        }
      }
  });
  dout("Finished T7\n");
}

// Okay, now remove any u-nodes (nodes that we don't directly care about) which
// don't effect any r-nodes (nodes that we do care about)
// To do this we do a reverse topological visit of the graph from each R node,
// and mark all visited nodes as needed.  We then remove any unmarked nodes.
static void T6(CFG::ControlFlowGraph &G) {
  std::set<CFG::NodeID> visited;
  // For each R node
  dout("Running T6\n");
  std::for_each(std::begin(G), std::end(G),
      [&G, &visited](CFG::ControlFlowGraph::node_iter_type &pnode) {
    auto &node = llvm::cast<CFG::Node>(*pnode);
    // Only deal with marked non-rep nodes
    // Mark the reverse topolocial sort of each r-node
    // Note, we don't need to visit visited r nodes
    if (node.r() &&
        visited.find(node.id()) == std::end(visited)) {
      // Do a topological search in reverse
      // Add this node to visited...
      // llvm::dbgs() << "Visiting node: " << node.id() << "\n";
      visited.insert(node.id());
      std::for_each(
          G.topo_rbegin(node.id()),
          G.topo_rend(node.id()),
          [&G, &visited](CFG::NodeID visit_id) {
        visited.insert(visit_id);
      });
    }
  });

  // Figure out which nodes are unused
  std::set<CFG::NodeID> remove_set;
  std::for_each(std::begin(G), std::end(G),
      [&G, &visited, &remove_set]
      (CFG::ControlFlowGraph::node_iter_type &pnode) {
    CFG::NodeID id = pnode->id();
    if (visited.find(id) == std::end(visited)) {
      remove_set.insert(id);
    }
  });

  // Remove any nodes not marked as needed
  std::for_each(std::begin(remove_set), std::end(remove_set),
      [&G](CFG::NodeID rm_id) {
    // llvm::dbgs() << "Removing node: " << rm_id << "\n";
    G.tryRemoveNode(rm_id);
  });
  dout("Finished T6\n");
}

// merge all up-nodes with exactly one successor with their successor
static void T5(CFG::ControlFlowGraph &G) {
  dout("Running T5\n");

  // Get a topological ordering of all up-nodes
  // For each up-node in said ordering
  // Create a new graph, with only up-nodes
  // Start with Gup as a clone of G

  CFG::ControlFlowGraph Gup = G.clone<CFG::Node>();

  // Now, remove any non-up nodes
  std::vector<CFG::NodeID> remove_list;
  std::for_each(std::begin(Gup), std::end(Gup),
      [&remove_list, &Gup](CFG::ControlFlowGraph::node_iter_type &pnode) {
    auto &node = llvm::cast<CFG::Node>(*pnode);
    // Note any non-up node to be removed post iteration
    if (!node.u() || !node.p()) {
      remove_list.push_back(node.id());
    }
  });

  /*
  assert(std::unique(std::begin(remove_list), std::end(remove_list)) ==
      std::end(remove_list));
  */

  // Remove any non-up-nodes from Gup
  std::for_each(std::begin(remove_list), std::end(remove_list),
      [&Gup] (CFG::NodeID id) {
    // llvm::dbgs() << "Removing node: " << id << "\n";
    Gup.removeNode(id);
  });

  // Now, visit each up-node in G in a topological order with repsect to the
  //     up-nodes -- We use Gup for this
  std::vector<CFG::NodeID> unite_ids;
  std::for_each(Gup.topo_begin(), Gup.topo_end(),
      [&G, &unite_ids](CFG::NodeID topo_id) {
    auto &nd = G.getNode<CFG::Node>(topo_id);
    // This had better be a up-node...
    assert(nd.isRep());
    assert(nd.u() && nd.p());

    // If the node has a unique successor:
    if (nd.succs().size() == 1) {
      // NOTE: I can't unite in the loop, it will screw with my iteration...
      // so I create a unite list to do later
      unite_ids.push_back(nd.id());
    }
  });

  // Now, unite any note that was denoted as being united
  std::for_each(std::begin(unite_ids), std::end(unite_ids),
      [&G](CFG::NodeID unite_id) {
    auto &unite_node = G.getNode<CFG::Node>(unite_id);

    // NOTE: The below assertion doesn't work, because I may have already united
    //   into the node
    /*
    if_debug_enabled(
      unite_node.removeDuplicateSuccs(G);
      assert(unite_node.succs().size() == 1));
    */

    auto &succ_id = *std::begin(unite_node.succs());
    auto &succ_node = G.getNode<CFG::Node>(succ_id);

    // we don't merge with ourself
    if (unite_node != succ_node) {
      // Remove the edge before we unite
      G.removeBidirEdge(succ_node.id(), unite_node.id());
      succ_node.unite(G, unite_node);
    }
  });

  dout("Finished T5\n");
}
//}}}

void Ramalingam(CFG::ControlFlowGraph &G) {
  // Start by restricting G to only p-nodes, this gives is "Gp"
  // Make Gp a copy of G
  if_debug(G.printDotFile("G.dot", *g_omap));

  dout("Creating Gp\n");
  CFG::ControlFlowGraph Gp = G.clone<CFG::Node>();

  if_debug(Gp.printDotFile("Gp_orig.dot", *g_omap));

  dout("  Creating remove list\n");
  std::vector<CFG::NodeID> remove_list;
  std::for_each(std::begin(Gp), std::end(Gp),
      [&remove_list]
      (CFG::ControlFlowGraph::node_iter_type &pnode) {
    auto &node = llvm::cast<CFG::Node>(*pnode);
    // If the node is non-preserving, remove it
    if (!node.p()) {
      remove_list.push_back(node.id());
    }
  });

  // Make the remove list unique
  std::sort(std::begin(remove_list), std::end(remove_list));
  auto it = std::unique(std::begin(remove_list), std::end(remove_list));
  remove_list.erase(it, std::end(remove_list));

  dout("  removing remove list\n");
  // Remove all non-preserving nodes from Gp
  std::for_each(std::begin(remove_list), std::end(remove_list),
      [&Gp](CFG::NodeID id) {
    Gp.removeNode(id);
  });

  if_debug(Gp.printDotFile("Gp.dot", *g_omap));

  // Now get the SCC version of Gp
  // NOTE: This will merge the nodes for me
  // We must clean edges before creating the SCC...
  dout("  Creating SCC\n");
  Gp.createSCC();
  dout("Gp Done\n");

  if_debug(Gp.printDotFile("Xp.dot", *g_omap));

  // T4 -- This transform collapses a set of strongly connected p (preserving)
  // nodes into a single node.
  T4(G, Gp);

  if_debug(G.printDotFile("G4.dot", *g_omap));

  // Similar to sfs's rm_undef -- but no removal of r nodes
  G.cleanGraph();

  // T2 -- If a node is a p-node and has precisely one predecessor, it may be
  // merged with that predecessor
  T2(G, Gp);

  if_debug(G.printDotFile("G2.dot", *g_omap));

  // For the remainder of the transformations we are concerned with calculating
  // a "Partially Equivalent Flow Graph" or a graph for which the data-flow
  // solution is only requried for some set of nodes (known henceforth as
  // r-nodes).  The set of nodes which the dataflow solution is not required is
  // a u-node, and preserving u-nodes are up-nodes

  // NOTE: We actually don't do T7, that is accounted for when we identify
  // objects (the alloc nodes are C nodes, which have no incoming edges)

  // For T7 we will add yet an additional class of nodes, the c-node.  If
  // a node is a m (modifying) node, but the modification function is a
  // constant, it is refered to as a c-node.

  // T7 -- If the c-node transformation is to delete the incoming edges, then we
  // delete the edges from the graph.
  // T7(G);

  if_debug(G.printDotFile("G7.dot", *g_omap));

  // T6 -- applys to any set of u-nodes without a successor (aka, the set of
  // nodes has no edge from a node to a node outside of the set).  We remove
  // the set X, as well as any edges incident on them.  (Incident in graph
  // theory means the edge is connected to a vertex, either in or out).
  T6(G);

  if_debug(G.printDotFile("G6.dot", *g_omap));

  // NOTE: T5 requires succ edges, we'll add them now:
  std::for_each(std::begin(G), std::end(G),
      [&G] (CFG::ControlFlowGraph::node_iter_type &pnode) {
    auto &preds = pnode->preds();
    auto succ_id = pnode->id();
    std::for_each(std::begin(preds), std::end(preds),
        [&G, &succ_id] (CFG::CFGid pred_id) {
      G.addSucc(pred_id, succ_id);
    });
  });

  // T5 -- merges any up-node with exactly one predecessor with its predecessor
  T5(G);

  if_debug(G.printDotFile("G5.dot", *g_omap));
}
//}}}

// Here we preform Ramalingam's algorithm from the paper "On Sparse Evaluation
// Representation" to create a SSA form of the graph.  This consists of a series
// of 5 transforms, T2,T4,T5,T6, and T7  The tramsforms are outlined in
// the paper.

// I suppose this requires knowing the set of nodes we care about... as we're
// calculating the "Partially Equivalent Flow Graph" (PEFG) representation
CFG::ControlFlowGraph
SpecSFS::computeSSA(const CFG::ControlFlowGraph &cfg) {
  // This essentially copies the CFG
  CFG::ControlFlowGraph ret = cfg.clone<CFG::Node>();

  /*
  llvm::dbgs() << "pre-Ramalingam: ret contains cfg ids:";
  std::for_each(ret.node_map_begin(), ret.node_map_end(),
      [] (std::pair<const CFG::CFGid, CFG::NodeID> &node_pair) {
      llvm::dbgs() << " " << node_pair.first;
  });
  llvm::dbgs() << "\n";

  llvm::dbgs() << "Initial nodeset is:\n";
  std::for_each(std::begin(ret), std::end(ret),
      [] (CFG::ControlFlowGraph::node_iter_type &pnode) {
    llvm::dbgs() << "  node " << pnode->id() << ": ";
    extern ObjectMap &g_omap;
    pnode->print_label(llvm::dbgs(), g_omap);
    llvm::dbgs() << "\n";
  });
  */

  Ramalingam(ret);

  /*
  llvm::dbgs() << "post-Ramalingam: ret contains cfg ids:";
  std::for_each(ret.node_map_begin(), ret.node_map_end(),
      [] (std::pair<const CFG::CFGid, CFG::NodeID> &node_pair) {
      llvm::dbgs() << " " << node_pair.first;
  });
  llvm::dbgs() << "\n";
  */

  return std::move(ret);
}

