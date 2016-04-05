/*
 * Copyright (C) 2015 David Devecsery
 */

#include <map>
#include <vector>

#include "include/DUG.h"
#include "include/ObjectMap.h"
#include "include/lib/UnusedFunctions.h"
#include "include/lib/IndirFcnTarget.h"

#include "llvm/Pass.h"
#include "llvm/PassSupport.h"
#include "llvm/Function.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ProfileInfo.h"
#include "llvm/Constants.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/Module.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/GetElementPtrTypeIterator.h"
#include "llvm/Support/InstIterator.h"

static llvm::cl::opt<bool>
  only_load_store("alias-load-store", llvm::cl::init(false),
      llvm::cl::value_desc("bool"),
      llvm::cl::desc("AliasTest will only test load-store values if true"));


class AliasTest : public llvm::ModulePass {
 public:
  static char ID;
  AliasTest() : llvm::ModulePass(ID) { }

  void getAnalysisUsage(llvm::AnalysisUsage &usage) const {
    usage.setPreservesAll();
    usage.addRequired<llvm::AliasAnalysis>();
    usage.addRequired<UnusedFunctions>();
  }

  bool runOnModule(llvm::Module &m) override {
    llvm::dbgs() << "AliasTest: Start\n";
    std::vector<const llvm::Value *> value_list;
    // Create a list of values to check aliases for
    auto &unused = getAnalysis<UnusedFunctions>();

    std::vector<const llvm::Instruction *> load_list;
    std::vector<const llvm::Instruction *> store_list;

    llvm::dbgs() << "AliasTest: Gathering Values\n";
    // GVs
    for (auto it = m.global_begin(), en = m.global_end(); it != en;
        ++it) {
      value_list.push_back(&(*it));
    }
    // Functions
    for (auto &fcn : m) {
      if (unused.isUsed(&fcn)) {
        value_list.push_back(&fcn);
        // Funciton arguments
        for (auto it = fcn.arg_begin(), en = fcn.arg_end();
            it != en; ++it) {
          if (llvm::isa<llvm::PointerType>(it->getType())) {
            value_list.push_back(&(*it));
          }
        }

        // Instructions
        for (auto &bb : fcn) {
          if (unused.isUsed(&bb)) {
            for (auto &inst : bb) {
              if (llvm::isa<llvm::PointerType>(inst.getType())) {
                value_list.push_back(&inst);
              }

              if (llvm::isa<llvm::LoadInst>(inst)) {
                load_list.push_back(&inst);
              } else if (llvm::isa<llvm::StoreInst>(inst)) {
                store_list.push_back(&inst);
              }
            }
          }
        }
      }
    }

    // Now that we have all of the "used" values in the program, find alias sets

    size_t num_checked = 0;
    size_t num_no_alias = 0;
    size_t num_must_alias = 0;
    size_t num_may_alias = 0;
    size_t num_values = value_list.size();
    auto &aa = getAnalysis<llvm::AliasAnalysis>();
    if (only_load_store) {
      for (auto li : load_list) {
        auto load_src = li->getOperand(0);
        for (auto si : store_list) {
          // Check if the load aliases with a store
          auto store_dest = si->getOperand(1);

          auto alias = aa.alias(llvm::AliasAnalysis::Location(load_src, 1),
               llvm::AliasAnalysis::Location(store_dest, 1));

          num_checked++;
          if (alias == llvm::AliasAnalysis::MayAlias) {
            num_may_alias++;
          }

          if (alias == llvm::AliasAnalysis::MustAlias) {
            num_must_alias++;
          }

          if (alias == llvm::AliasAnalysis::NoAlias) {
            num_no_alias++;
          }
        }
      }
    } else {
      llvm::dbgs() << "AliasTest: Counting Aliases\n";
      // Now, iterate the list
      // For each value to check
      {
        util::PerfTimerPrinter t(llvm::dbgs(), "Counting Aliases");
        for (auto it = std::begin(value_list), en = std::end(value_list);
            it != en; ++it) {
          auto base_val = *it;
          // Check against all values not yet checked (O(n^2))
          for (auto it2 = it+1; it2 != en; ++it2) {
            auto check_val = *it2;
            auto alias = aa.alias(llvm::AliasAnalysis::Location(base_val, 1),
                 llvm::AliasAnalysis::Location(check_val, 1));

            num_checked++;
            if (alias == llvm::AliasAnalysis::MayAlias) {
              num_may_alias++;
            }

            if (alias == llvm::AliasAnalysis::MustAlias) {
              num_must_alias++;
            }

            if (alias == llvm::AliasAnalysis::NoAlias) {
              num_no_alias++;
            }
          }
        }
      }
    }

    llvm::dbgs() << "AliasTest results:\n";
    llvm::dbgs() << "  Num values:     " << num_values << "\n";
    llvm::dbgs() << "  Num checks:     " << num_checked << "\n";
    llvm::dbgs() << "  Num must alias: " << num_must_alias << "\n";
    llvm::dbgs() << "  Num may  alias: " << num_may_alias << "\n";
    llvm::dbgs() << "  Num no   alias: " << num_no_alias << "\n";

    return false;
  }
};

char AliasTest::ID = 0;
static llvm::RegisterPass<AliasTest>
  X("alias-test", "AliasTest", false, false);

