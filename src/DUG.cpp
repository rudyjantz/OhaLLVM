/*
 * Copyright (C) 2015 David Devecsery
 */

#include "include/DUG.h"

#include <string>
#include <iostream>
#include <fstream>
#include <map>
#include <set>
#include <utility>

#define SPECSFS_DEBUG
#include "include/Debug.h"
#include "include/ObjectMap.h"

#include "llvm/Function.h"
#include "llvm/Support/raw_os_ostream.h"

// DUG Static variable(s) {{{
const DUG::CFGid DUG::CFGInit =
  DUG::CFGid(static_cast<int32_t>(DUG::CFGEnum::CFGInit));
//}}}

// DUG modification functions {{{
bool DUG::removeUnusedFunction(DUG::ObjID fcn_id) {
  auto it = unusedFunctions_.find(fcn_id);

  if (it != std::end(unusedFunctions_)) {
    for (auto id : it->second) {
      constraintGraph_.removeEdge(id);
    }

    unusedFunctions_.erase(fcn_id);

    // Successful removal
    return true;
  } else {
    // Unsuccessful removal
    return false;
  }
}
//}}}

