/*
 * Copyright (C) 2015 David Devecsery
 */


// Enable debugging prints for this file
#define SPECSFS_DEBUG
// #define SPECSFS_LOGDEBUG
// #define SEPCSFS_PRINT_RESULTS

#include "include/SpecSFS.h"

#include <execinfo.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ProfileInfo.h"


#include "include/Andersens.h"
#include "include/Debug.h"
#include "include/ObjectMap.h"
#include "include/lib/UnusedFunctions.h"
#include "include/lib/IndirFcnTarget.h"
#include "include/lib/DynPtsto.h"

using std::swap;

static llvm::cl::opt<std::string>
  fcn_name("specsfs-debug-fcn", llvm::cl::init(""),
      llvm::cl::value_desc("string"),
      llvm::cl::desc("if set specsfs will print the ptsto set for this function"
        " at the end of execution"));

static llvm::cl::opt<std::string>
  glbl_name("specsfs-debug-glbl", llvm::cl::init(""),
      llvm::cl::value_desc("string"),
      llvm::cl::desc("if set specsfs will print the ptsto set for this global"
        " at the end of execution"));

// Error handling functions
/*{{{*/
// Don't warn about this (if it is an) unused function... I'm being sloppy
__attribute__((unused))
static void print_stack_trace(void) {
  void *array[10];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, 10);
  strings = backtrace_symbols(array, size);

  llvm::errs() << "BACKTRACE:\n";
  for (i = 0; i < size; i++) {
    llvm::errs() << "\t" << strings[i] << "\n";
  }

  free(strings);
}

static void error(const std::string &msg) {
  llvm::errs() << "ERROR: " << msg << "\n";
  print_stack_trace();
  assert(0);
}
/*}}}*/

// Constructor
SpecSFS::SpecSFS() : llvm::ModulePass(ID) { }
SpecSFS::SpecSFS(char &id) : llvm::ModulePass(id) { }
char SpecSFS::ID = 0;
namespace llvm {
  static RegisterPass<SpecSFS>
      X("SpecSFS", "Speculative Sparse Flow-sensitive Analysis", false, true);
  RegisterAnalysisGroup<AliasAnalysis> Y(X);
}  // namespace llvm

using llvm::PassRegistry;
using llvm::PassInfo;
using llvm::callDefaultCtor;
using llvm::AliasAnalysis;

void SpecSFS::getAnalysisUsage(llvm::AnalysisUsage &usage) const {
  // Because we're an AliasAnalysis
  // AliasAnalysis::getAnalysisUsage(usage);
  usage.addRequired<llvm::AliasAnalysis>();
  usage.setPreservesAll();
  // For DCE
  // FIXME: This info is actually provided by ProfileInfo?
  usage.addRequired<UnusedFunctions>();
  // For indirect function following
  // FIXME: Can probably just use DynPtstoLoader...
  // usage.addRequired<IndirFunctionInfo>();
  // For dynamic ptsto removal
  // usage.addRequired<DynPtstoLoader>();
  usage.addRequired<llvm::ProfileInfo>();
}

// runOnModule, the primary pass
bool SpecSFS::runOnModule(llvm::Module &m) {
  // Set up our alias analysis
  // -- This is required for the llvm AliasAnalysis interface
  InitializeAliasAnalysis(this);

  if (fcn_name != "") {
    llvm::dbgs() << "Got debug function: " << fcn_name << "\n";
  }
  if (glbl_name != "") {
    llvm::dbgs() << "Got debug gv: " << glbl_name << "\n";
  }


  // Clear the def-use graph
  // It should already be cleared, but I'm paranoid
  ConstraintGraph cg;
  CFG cfg;
  ObjectMap &omap = omap_;

  const UnusedFunctions &unused_fcns =
      getAnalysis<UnusedFunctions>();

  ObjectMap::replaceDbgOmap(omap);
  {
    PerfTimerPrinter id_timer(llvm::dbgs(), "Identify Objects");
    // Identify all of the objects in the source
    if (identifyObjects(omap, m)) {
      error("Identify objects failure!");
    }
  }

  // Get the initial (top-level) constraints set
  // This should also track the def/use info
  // NOTE: This function will create a graph of all top-level variables,
  //   and a def/use mapping, but it will not fill in address-taken edges.
  //   Those will be populated later, once we have AUX info available.
  {
    PerfTimerPrinter create_timer(llvm::dbgs(), "CreateConstraints");
    if (createConstraints(cg, cfg, omap, m, unused_fcns)) {
      error("CreateConstraints failure!");
    }
  }

  // Initial optimization pass
  // Runs HU on the graph as it stands, w/ only top level info filled in
  // Removes any nodes deemed to be non-ptr (definitely null), and merges nodes
  //   with statically equivalent ptsto sets
  {
    PerfTimerPrinter opt_timer(llvm::dbgs(), "optimizeConstarints");
    if (optimizeConstraints(cg, cfg, omap)) {
      error("OptimizeConstraints failure!");
    }
  }

  if_debug(cfg.getSEG().printDotFile("CFG.dot", omap));

  cfg.cleanup();

  Andersens aux;
  // Get AUX info, in this instance we choose Andersens
  {
    PerfTimerPrinter andersens_timer(llvm::dbgs(), "Andersens");
    if (aux.runOnModule(m)) {
      // Andersens had better not change m!
      error("Andersens changes m???");
    }
  }

  {
    // Converting from aux_id to obj_ids
    // For each pointer value we care about:
    // dout("Filling aux_to_obj:\n");
    auto &aux_val_nodes = aux.getObjectMap();

    special_aux_.emplace(ObjectMap::NullValue, Andersens::NullPtr);
    special_aux_.emplace(ObjectMap::NullObjectValue, Andersens::NullObject);
    special_aux_.emplace(ObjectMap::ArgvValue, Andersens::ArgvValue);
    special_aux_.emplace(ObjectMap::ArgvObjectValue, Andersens::ArgvObject);
    special_aux_.emplace(ObjectMap::IntValue, aux.IntNode);
    special_aux_.emplace(ObjectMap::UniversalValue, Andersens::UniversalSet);
    special_aux_.emplace(ObjectMap::LocaleObject, Andersens::LocaleObject);
    special_aux_.emplace(ObjectMap::CTypeObject, Andersens::CTypeObject);
    special_aux_.emplace(ObjectMap::ErrnoObject, Andersens::ErrnoObject);
    special_aux_.emplace(ObjectMap::PthreadSpecificValue,
        aux.PthreadSpecificNode);

    aux_to_obj_.emplace(Andersens::NullPtr, ObjectMap::NullValue);
    aux_to_obj_.emplace(Andersens::NullObject, ObjectMap::NullObjectValue);
    aux_to_obj_.emplace(Andersens::ArgvValue, ObjectMap::ArgvValue);
    aux_to_obj_.emplace(Andersens::ArgvObject, ObjectMap::ArgvObjectValue);
    aux_to_obj_.emplace(aux.IntNode, ObjectMap::IntValue);
    aux_to_obj_.emplace(Andersens::UniversalSet, ObjectMap::UniversalValue);
    aux_to_obj_.emplace(Andersens::LocaleObject, ObjectMap::LocaleObject);
    aux_to_obj_.emplace(Andersens::CTypeObject, ObjectMap::CTypeObject);
    aux_to_obj_.emplace(Andersens::ErrnoObject, ObjectMap::ErrnoObject);
    aux_to_obj_.emplace(aux.PthreadSpecificNode,
        ObjectMap::PthreadSpecificValue);

    std::for_each(std::begin(aux_val_nodes), std::end(aux_val_nodes),
        [this, &aux, &omap]
        (const std::pair<const llvm::Value *, uint32_t> &pr) {
      auto obj_id = omap.getObject(pr.first);
      // auto obj_id = omap.getValue(pr.first);
      auto aux_val_id = pr.second;

      /*
      dout("  " << aux_val_id << "->" << obj_id <<
        "\n");
      */
      assert(aux_to_obj_.find(aux_val_id) == std::end(aux_to_obj_));
      aux_to_obj_.emplace(aux_val_id, obj_id);
    });
  }

  // Now, fill in the indirect function calls
  {
    PerfTimerPrinter indir_timer(llvm::dbgs(), "addIndirectCalls");
    // const auto &dyn_indir_info = getAnalysis<IndirFunctionInfo>();
    IndirFunctionInfo *pass = nullptr;
    // if (addIndirectCalls(cg, cfg, aux, &dyn_indir_info, omap))
    if (addIndirectCalls(cg, cfg, aux, pass, omap)) {
      error("AddIndirectCalls failure!");
    }
  }

  // Now that we've resolve our indir edges, we can remove duplicate constraints
  // (possibly created by optimizeConstraints())
  {
    PerfTimerPrinter unique_timer(llvm::dbgs(), "cg.unique()");
  }

  // if_debug(cfg.getSEG().printDotFile("CFG_indir.dot", omap));
  // cfg.getSEG().printDotFile("CFG_indir.dot", omap);

  // Now, compute the SSA form for the top-level variables
  // We translate any PHI nodes into copy nodes... b/c the paper says so
  {
    PerfTimerPrinter ssa_timer(llvm::dbgs(), "computeSSA");
    computeSSA(cfg.getSEG());
  }

  // cfg.getSEG().printDotFile("CFG_ssa.dot", omap);

  // Compute partitions, based on address equivalence
  DUG graph;
  {
    PerfTimerPrinter fill_timer(llvm::dbgs(), "fillTopLevel");
    graph.fillTopLevel(cg, omap);
  }

  /*
  // Now that we've filled in the top level constraint graph, we add in dynamic
  //   info
  {
    PerfTimerPrinter dyn_timer(llvm::dbgs(), "Dynamic Ptsto Info");
    if (addDynPtstoInfo(m, graph, cfg, omap)) {
      error("DynPtstoInfo addition failure!");
    }
  }
  */

  {
    PerfTimerPrinter partition_timer(llvm::dbgs(), "computePartitions");
    if (computePartitions(graph, cfg, aux, omap)) {
      error("ComputePartitions failure!");
    }
  }

  // Compute SSA for each partition
  {
    PerfTimerPrinter part_dug_timer(llvm::dbgs(), "addPartitionsToDUG");
    if (addPartitionsToDUG(graph, cfg, omap)) {
      error("ComputePartSSA failure!");
    }
  }

  graph.setStructInfo(omap.getIsStructSet());

  // Finally, solve the graph
  {
    PerfTimerPrinter solve_timer(llvm::dbgs(), "solve");
    if (solve(graph, omap)) {
      error("Solve failure!");
    }
  }


  // Go through a function, and print the ptsto graph for that function, this
  // should be faster then printing the whole graph
  if (glbl_name != "") {
    llvm::dbgs() << "Printing ptsto for global variable: " << glbl_name << "\n";
    auto glbl = m.getNamedValue(glbl_name);
    auto val_id = omap.getValue(glbl);
    auto &pts_set_vec = pts_top_.atVec(val_id);


    llvm::dbgs() << "pts_top[" << val_id << "]: " << ValPrint(val_id) <<
      "\n";

    for (uint32_t i = 0; i < pts_set_vec.size(); i++) {
      llvm::dbgs() << "  Offset: " << i << "\n";

      auto &ptsto = pts_set_vec[i];

      std::for_each(ptsto.cbegin(), ptsto.cend(),
          [&omap] (const ObjectMap::ObjID obj_id) {
        llvm::dbgs() << "    " << obj_id << ": " << ValPrint(obj_id)
            << "\n";
      });
    }
  }

  if (fcn_name != "") {
    auto fcn = m.getFunction(fcn_name);

    llvm::dbgs() << "Printing ptsto for function: " << fcn_name << "\n";
    std::for_each(inst_begin(fcn), inst_end(fcn),
        [this, &omap] (llvm::Instruction &inst) {
      if (llvm::isa<llvm::PointerType>(inst.getType())) {
        auto val_id = omap.getValue(&inst);
        auto &pts_set_vec = pts_top_.atVec(val_id);

        llvm::dbgs() << "pts_top[" << val_id << "]: " << ValPrint(val_id) <<
          "\n";

        for (uint32_t i = 0; i < pts_set_vec.size(); i++) {
          llvm::dbgs() << "  Offset: " << i << "\n";

          auto &ptsto = pts_set_vec[i];

          std::for_each(ptsto.cbegin(), ptsto.cend(),
              [&omap] (const ObjectMap::ObjID obj_id) {
            llvm::dbgs() << "    " << obj_id << ": " << ValPrint(obj_id)
                << "\n";
          });
        }
      }
    });
  }

#if !defined(SPECSFS_NODEBUG) && defined(SPECSFS_PRINT_RESULTS)
  llvm::dbgs() << "Printing the rep mappings for top level variales:\n";
  for (auto &pr : obj_to_rep_) {
    auto obj_id = pr.first;
    auto rep_id = getObjIDRep(obj_id);

    llvm::dbgs() << obj_id << "->" << rep_id << "\n";
  }


  llvm::dbgs() << "Printing final ptsto set for top level variables:\n";
  std::for_each(pts_top_.cbegin(), pts_top_.cend(),
      [&omap]
      (const TopLevelPtsto::PtsPair &pr) {
    auto top_val = omap.valueAtID(pr.id());

    if (omap.isObject(pr.id())) {
      llvm::dbgs() << "Object ";
    } else {
      llvm::dbgs() << "Value ";
    }

    if (top_val == nullptr) {
      llvm::dbgs() << "is (id): " << pr.id() << "\n";
    } else if (auto gv = dyn_cast<llvm::GlobalValue>(top_val)) {
      llvm::dbgs() << "(" << pr.id() << ") is: " <<
          gv->getName() << "\n";
    } else if (auto fcn = dyn_cast<llvm::Function>(top_val)) {
      llvm::dbgs() << "(" << pr.id() <<") is: " <<
          fcn->getName() << "\n";
    } else {
      llvm::dbgs() << "(" << pr.id() << ") is: " << *top_val << "\n";
    }

    for (uint32_t i = 0; i < pr.pts().size(); i++) {
      llvm::dbgs() << "  Offset: " << i << "\n";

      // Statistics
      auto &ptsto = pr.pts()[i];

      // Printing
      std::for_each(ptsto.cbegin(), ptsto.cend(),
          [&omap] (const ObjectMap::ObjID obj_id) {
        auto loc_val = omap.valueAtID(obj_id);

        if (loc_val == nullptr) {
          llvm::dbgs() << "    Value is (id): " << obj_id << "\n";
        } else if (auto gv = dyn_cast<llvm::GlobalValue>(loc_val)) {
          llvm::dbgs() << "    " << obj_id << ": " << gv->getName() << "\n";
        } else if (auto fcn = dyn_cast<llvm::Function>(loc_val)) {
          llvm::dbgs() << "    " << obj_id << ": " << fcn->getName() << "\n";
        } else {
          llvm::dbgs() << "    " << obj_id << ": " << *loc_val << "\n";
        }
      });
    }
  });
#endif

#ifndef SPECSFS_NOSTATS
  // STATS!
  int64_t total_variables = 0;
  int64_t total_ptstos = 0;

  int32_t num_objects[10] = {};

  size_t max_objects = 0;
  int32_t num_max = 0;

  std::for_each(pts_top_.cbegin(), pts_top_.cend(),
      [&omap, &total_variables, &total_ptstos, &max_objects, &num_objects,
        &num_max]
      (const TopLevelPtsto::PtsPair &pr) {
    for (uint32_t i = 0; i < pr.pts().size(); i++) {
      // Statistics
      auto &ptsto = pr.pts()[i];
      size_t ptsto_size = ptsto.size();

      total_variables++;
      total_ptstos += ptsto_size;

      if (ptsto_size < 10) {
        num_objects[ptsto_size]++;
      }

      if (ptsto_size > max_objects) {
        max_objects = ptsto_size;
        num_max = 0;
      }

      if (ptsto_size == max_objects) {
        num_max++;
      }
    }
  });

  llvm::dbgs() << "Number tracked values: " << total_variables << "\n";
  llvm::dbgs() << "Number tracked ptstos: " << total_ptstos << "\n";

  llvm::dbgs() << "Max ptsto is: " << max_objects << ", with num_max: " <<
    num_max << "\n";

  llvm::dbgs() << "lowest ptsto counts:\n";
  for (int i = 0; i < 10; i++) {
    llvm::dbgs() << "  [" << i << "]:  " << num_objects[i] << "\n";
  }
#endif

  /*
  if (alias(Location(nullptr), Location(nullptr)) != MayAlias) {
    llvm::dbgs() << "?\n";
  }
  */

  // We do not modify code, ever!
  return false;
}

llvm::AliasAnalysis::AliasResult SpecSFS::alias(const Location &L1,
                                            const Location &L2) {
  auto obj_id1 = omap_.getValue(L1.Ptr);
  auto obj_id2 = omap_.getValue(L2.Ptr);
  auto rep_id1 = getObjIDRep(obj_id1);
  auto rep_id2 = getObjIDRep(obj_id2);
  auto pts1_it = pts_top_.find(rep_id1);
  auto pts2_it = pts_top_.find(rep_id2);

  if (pts1_it == std::end(pts_top_)) {
    return AliasAnalysis::alias(L1, L2);
  }

  if (pts2_it == std::end(pts_top_)) {
    return AliasAnalysis::alias(L1, L2);
  }

  auto &pts1 = pts1_it->pts();
  auto &pts2 = pts2_it->pts();

  // If either of the sets point to nothing, no alias
  if (pts1.empty() || pts2.empty()) {
    return NoAlias;
  }

  // Check to see if the two pointers are known to not alias.  They don't alias
  // if their points-to sets do not intersect.
  if (!pts1.front().insersectsIgnoring(pts2.front(),
        ObjectMap::NullObjectValue)) {
    return NoAlias;
  }

  return AliasAnalysis::alias(L1, L2);
}

AliasAnalysis::ModRefResult SpecSFS::getModRefInfo(llvm::ImmutableCallSite CS,
                                   const llvm::AliasAnalysis::Location &Loc) {
  return AliasAnalysis::getModRefInfo(CS, Loc);
}

AliasAnalysis::ModRefResult SpecSFS::getModRefInfo(llvm::ImmutableCallSite CS1,
                                   llvm::ImmutableCallSite CS2) {
  return AliasAnalysis::getModRefInfo(CS1, CS2);
}

/*
void SpecSFS::getMustAliases(llvm::Value *P, std::vector<llvm::Value*> &RetVals) {
  return AliasAnalysis::getMustAliases(P, RetVals);
}
*/

// Do not use it.
bool SpecSFS::pointsToConstantMemory(const AliasAnalysis::Location &loc,
    bool or_local) {
  return AliasAnalysis::pointsToConstantMemory(loc, or_local);
}

void SpecSFS::deleteValue(llvm::Value *V) {
  // FIXME: Should do this
  // Really, I'm just going to ignore it...
  auto v_id = omap_.getValue(V);
  llvm::dbgs() << "Deleting value: " << v_id << "\n";
  pts_top_.remove(v_id);
  getAnalysis<AliasAnalysis>().deleteValue(V);
}

void SpecSFS::copyValue(llvm::Value *From, llvm::Value *To) {
  // FIXME: Should do this
  auto from_id = omap_.getValue(From);
  auto to_id = omap_.getValue(To);
  pts_top_.copy(from_id, to_id);

  getAnalysis<AliasAnalysis>().copyValue(From, To);
}


