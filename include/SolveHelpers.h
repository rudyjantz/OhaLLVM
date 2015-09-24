/*
 * Copyright (C) 2015 David Devecsery
 */

#ifndef INCLUDE_SOLVEHELPERS_H_
#define INCLUDE_SOLVEHELPERS_H_

#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <utility>
#include <vector>

#include "llvm/ADT/SparseBitVector.h"


// Bitmap used in many places (and by Andersen's) to represent ptsto
typedef llvm::SparseBitVector<> Bitmap;

// Required for using a Bitmap as a std::map key (used for ID gathering) {{{
struct BitmapLT {
  bool operator() (const Bitmap &b1, const Bitmap &b2) {
    auto it1 = std::begin(b1);
    auto it2 = std::begin(b2);
    auto en1 = std::end(b1);
    auto en2 = std::end(b2);

    // True if any element b1 < b2
    for (; it1 != en1 && it2 != en2; ++it1, ++it2) {
      // if it1 < it2 : b1 < b2
      if (*it1 < *it2) {
        return true;
      // If it1 > it2 : b1 > b2
      } else if (*it1 > *it2) {
        return false;
      }
      // Otherwise, they are equal, try the next element
    }

    // If they are equivalent but b1 is longer b1 is less than it b2
    if (it1 != en1) {
      return true;
    }

    return false;
  }
};
//}}}

class DUG;
class DUGNode;

// Typedef for PartID
struct part_id { };
typedef ID<part_id, int32_t> __PartID;

// FIXME: Build a better (read - priority) work queue
class Worklist {
  //{{{
 public:
    DUGNode *pop() {
      if (q_.empty()) {
        return nullptr;
      }
      auto ret = q_.front();
      q_.pop();
      return ret;
    }

    void push(DUGNode *pnd) {
      q_.push(pnd);
    }

 private:
    std::queue<DUGNode *> q_;
  //}}}
};

// FIXME: BDDs
class PtstoSet {
  //{{{
 public:
    typedef typename SEG::NodeID NodeID;
    bool set(ObjectMap::ObjID id) {
      return ptsto_.test_and_set(id.val());
    }

    bool assign(const PtstoSet &rhs) {
      bool ret = (ptsto_ == rhs.ptsto_);

      ptsto_.clear();
      ptsto_ |= rhs.ptsto_;

      return ret;
    }

    void clear() {
      ptsto_.clear();
    }

    bool operator|=(const PtstoSet &rhs) {
      return ptsto_ |= rhs.ptsto_;
    }

    bool operator|=(ObjectMap::ObjID &id) {
      return ptsto_.test_and_set(id.val());
    }

    bool orOffs(const PtstoSet &rhs, int32_t offs,
        const std::map<ObjectMap::ObjID, int32_t> struct_set) {
      bool ret = false;
      std::for_each(std::begin(rhs.ptsto_), std::end(rhs.ptsto_),
          [this, &ret, &offs, &struct_set]
          (const int32_t &val) {
        auto or_offs = offs;
        // If this isn't a structure, don't treat it with an offset
        auto it = struct_set.find(ObjectMap::ObjID(val));
        if (it == std::end(struct_set) || it->second <= or_offs) {
          or_offs = 0;
        }
        ret |= ptsto_.test_and_set(val + or_offs);
      });

      return ret;
    }

    bool insersectsIgnoring(PtstoSet &rhs, ObjectMap::ObjID ignore) {
      bool ret = false;

      bool lhs_add = ptsto_.test(ignore.val());
      bool rhs_add = rhs.ptsto_.test(ignore.val());

      if (lhs_add) {
        ptsto_.reset(ignore.val());
      }

      if (rhs_add) {
        rhs.ptsto_.reset(ignore.val());
      }

      ret = ptsto_.intersects(rhs.ptsto_);

      if (lhs_add) {
        ptsto_.set(ignore.val());
      }

      if (rhs_add) {
        rhs.ptsto_.set(ignore.val());
      }


      return ret;
    }

    size_t size() const {
      return ptsto_.count();
    }

    class iterator {
      //{{{
     public:
        typedef std::forward_iterator_tag iterator_category;
        typedef ObjectMap::ObjID value_type;
        typedef int32_t difference_type;
        typedef ObjectMap::ObjID * pointer;
        typedef ObjectMap::ObjID & reference;

        // Constructor {{{
        explicit iterator(Bitmap::iterator itr) : itr_(itr) { }
        //}}}

        // Operators {{{
        bool operator==(const iterator &it) const {
          return (itr_ == it.itr_);
        }

        bool operator!=(const iterator &it) const {
          return !operator==(it);
        }

        value_type operator*() const {
          return ObjectMap::ObjID(itr_.operator*());
        }

        /*
        value_type operator->() const {
          return ObjectMap::ObjID(itr_.operator->());
        }
        */

        iterator &operator++() {
          ++itr_;
          return *this;
        }

        iterator operator++(int) {
          iterator tmp(*this);
          ++itr_;

          return std::move(tmp);
        }
        //}}}

     private:
        // Private data {{{
        Bitmap::iterator itr_;
        //}}}
      //}}}
    };

    class const_iterator {
      //{{{
     public:
        typedef std::forward_iterator_tag iterator_category;
        typedef ObjectMap::ObjID value_type;
        typedef int32_t difference_type;
        typedef ObjectMap::ObjID * pointer;
        typedef ObjectMap::ObjID & reference;

        // Constructor {{{
        explicit const_iterator(Bitmap::iterator itr) : itr_(itr) { }
        //}}}

        // Operators {{{
        bool operator==(const const_iterator &it) const {
          return (itr_ == it.itr_);
        }

        bool operator!=(const const_iterator &it) const {
          return !operator==(it);
        }

        const value_type operator*() const {
          return ObjectMap::ObjID(itr_.operator*());
        }

        /*
        value_type operator->() const {
          return ObjectMap::ObjID(itr_.operator->());
        }
        */

        const_iterator &operator++() {
          ++itr_;
          return *this;
        }

        const_iterator operator++(int) {
          const_iterator tmp(*this);
          ++itr_;

          return std::move(tmp);
        }
        //}}}

     private:
        // Private data {{{
        Bitmap::iterator itr_;
        //}}}
      //}}}
    };

    iterator begin() {
      return iterator(std::begin(ptsto_));
    }

    iterator end() {
      return iterator(std::end(ptsto_));
    }

    const_iterator begin() const {
      return const_iterator(std::begin(ptsto_));
    }

    const_iterator end() const {
      return const_iterator(std::end(ptsto_));
    }

    const_iterator cbegin() const {
      return const_iterator(std::begin(ptsto_));
    }

    const_iterator cend() const {
      return const_iterator(std::end(ptsto_));
    }

    friend llvm::raw_ostream &operator<<(llvm::raw_ostream &os,
        const PtstoSet &ps) {
      os << "{";
      std::for_each(std::begin(ps), std::end(ps),
          [&os] (ObjectMap::ObjID id) {
        os << " " << id;
      });
      os << " }";

      return os;
    }

 private:
    Bitmap ptsto_;
  //}}}
};

class TopLevelPtsto {
  //{{{
 public:
    typedef typename SEG::NodeID NodeID;
    typedef ObjectMap::ObjID ObjID;

    TopLevelPtsto() = default;

    explicit TopLevelPtsto(const std::vector<ObjID> &objs) {
      std::for_each(std::begin(objs), std::end(objs),
          [this] (const ObjID &id) {
        data_.emplace(id, std::vector<PtstoSet>());
      });
    }

    // Allow move assignment {{{
    TopLevelPtsto(TopLevelPtsto &&) = delete;
    // Allow copying... needed in store?
    TopLevelPtsto(const TopLevelPtsto &) = default;

    TopLevelPtsto &operator=(TopLevelPtsto &&) = default;
    TopLevelPtsto &operator=(const TopLevelPtsto &) = delete;
    //}}}

    PtstoSet &at(ObjID id, int32_t offset) {
      auto &vec = data_.at(id);

      assert(offset >= 0);
      if (vec.size() < (uint32_t)offset+1) {
        vec.resize(offset+1);
      }

      auto &ret = vec.at(offset);

      return ret;
    }

    PtstoSet &at(ObjID id) {
      return at(id, 0);
    }

    typedef std::map<ObjID, std::vector<PtstoSet>>::iterator iterator;
    typedef std::map<ObjID, std::vector<PtstoSet>>::const_iterator
      const_iterator;

    iterator begin() {
      return std::begin(data_);
    }

    iterator end() {
      return std::end(data_);
    }

    const_iterator begin() const {
      return std::begin(data_);
    }

    const_iterator end() const {
      return std::end(data_);
    }

    const_iterator cbegin() const {
      return data_.cbegin();
    }

    const_iterator cend() const {
      return data_.cend();
    }

    friend llvm::raw_ostream &operator<<(llvm::raw_ostream &o,
        const TopLevelPtsto &g) {
      o << "( ";
      bool first = true;
      for (auto &pr : g) {
        if (!first) {
          o << ", ";
        }

        for (auto &pts_set : pr.second) {
          o << pr.first << "->" << pts_set;
        }
        first = false;
      }
      o << " )";
      return o;
    }

 private:
    std::map<ObjID, std::vector<PtstoSet>> data_;
  //}}}
};

// This is fine?
// FIXME: Need to have a way to add objects to our ptsto graph
class PtstoGraph {
  //{{{
 public:
    typedef typename SEG::NodeID NodeID;
    typedef ObjectMap::ObjID ObjID;

    PtstoGraph() = default;

    explicit PtstoGraph(const std::vector<ObjID> &objs) {
      std::for_each(std::begin(objs), std::end(objs),
          [this] (const ObjID &id) {
        data_.emplace(id, PtstoSet());
      });
    }

    // Allow move assignment {{{
    PtstoGraph(PtstoGraph &&) = delete;
    // Allow copying... needed in store?
    PtstoGraph(const PtstoGraph &) = default;

    PtstoGraph &operator=(PtstoGraph &&) = default;
    PtstoGraph &operator=(const PtstoGraph &) = delete;
    //}}}

    PtstoSet &at(ObjID id) {
      return data_.at(id);
    }

    bool operator|=(PtstoGraph &rhs) {
      // Oh goody...
      bool ret = false;
      std::for_each(std::begin(rhs.data_), std::end(rhs.data_),
          [this, &ret]
          (std::pair<const ObjID, PtstoSet> &pr) {
        auto &rhs_ptsset = pr.second;
        auto &lhs_ptsset = data_.at(pr.first);

        ret |= (lhs_ptsset |= rhs_ptsset);
      });

      return ret;
    }

    bool assign(ObjID elm, const PtstoSet &ptsset) {
      return data_.at(elm).assign(ptsset);
    }

    bool orPart(const PtstoSet &rhs,
        std::map<ObjID, __PartID> &part_map, __PartID part_id) {
      bool ret = false;
      std::for_each(std::begin(data_), std::end(data_),
          [this, &ret, &rhs, &part_id, &part_map]
          (std::pair<const ObjID, PtstoSet> &pr) {
        auto obj_id = pr.first;

        if (part_id == part_map.at(obj_id)) {
          auto &lhs_ptsset = pr.second;
          ret |= (lhs_ptsset |= rhs);
        }
      });

      return ret;
    }

    bool orElement(ObjID elm, const PtstoSet &rhs) {
      bool ret = false;

      auto &lhs_ptsset = data_.at(elm);
      ret |= (lhs_ptsset |= rhs);

      return ret;
    }

    bool orExcept(PtstoGraph &rhs,
        ObjID exception) {
      bool ret = false;

      std::for_each(std::begin(data_), std::end(data_),
          [this, &ret, &rhs, &exception]
          (std::pair<const ObjID, PtstoSet> &pr) {
        auto obj_id = pr.first;
        if (obj_id != exception) {
          auto &lhs_ptsset = pr.second;
          auto &rhs_ptsset = rhs.data_.at(obj_id);

          ret |= (lhs_ptsset |= rhs_ptsset);
        }
      });

      return ret;
    }

    bool orPart(PtstoGraph &rhs,
        std::map<ObjID, __PartID> &part_map, __PartID part_id) {
      bool ret = false;
      std::for_each(std::begin(data_), std::end(data_),
          [this, &ret, &rhs, &part_id, &part_map]
          (std::pair<const ObjID, PtstoSet> &pr) {
        auto obj_id = pr.first;
        if (part_id == part_map.at(obj_id)) {
          auto &lhs_ptsset = pr.second;
          auto &rhs_ptsset = rhs.data_.at(obj_id);

          ret |= (lhs_ptsset |= rhs_ptsset);
        }
      });

      return ret;
    }

    typedef std::map<ObjID, PtstoSet>::iterator iterator;
    typedef std::map<ObjID, PtstoSet>::const_iterator
      const_iterator;

    iterator begin() {
      return std::begin(data_);
    }

    iterator end() {
      return std::end(data_);
    }

    const_iterator begin() const {
      return std::begin(data_);
    }

    const_iterator end() const {
      return std::end(data_);
    }

    const_iterator cbegin() const {
      return data_.cbegin();
    }

    const_iterator cend() const {
      return data_.cend();
    }

    friend llvm::raw_ostream &operator<<(llvm::raw_ostream &o,
        const PtstoGraph &g) {
      o << "( ";
      bool first = true;
      for (auto &pr : g) {
        if (!first) {
          o << ", ";
        }

        o << pr.first << "->" << pr.second;

        first = false;
      }
      o << " )";
      return o;
    }

 private:
    std::map<ObjID, PtstoSet> data_;
  //}}}
};

#endif  // INCLUDE_SOLVEHELPERS_H_
