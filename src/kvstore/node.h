#ifndef ZLOG_KVSTORE_NODE_H
#define ZLOG_KVSTORE_NODE_H
#include <cassert>
#include <memory>
#include <string>
#include <iostream>
#include <vector>
#include <mutex>
#include <zlog/slice.h>
#include "kvstore.pb.h"

class Node;
using SharedNodeRef = std::shared_ptr<Node>;

class DBImpl;

/*
 * The read-only flag is a temporary hack for enforcing read-only property on
 * the connected Node. What is really needed is a more sophisticated approach
 * that avoids duplicating the read-only flag as well as what is probably some
 * call overhead associated with this design. Overall, this isn't pretty but
 * lets us have confidence in the correctness which is the priority right now.
 * There is probably a lot of overhead always returning copies of the
 * shared_ptr NodeRef.
 *
 * The NodePtr can point to Nil, which is represented by a NodeRef singleton.
 * In this case ref() will resolve to the singleton heap address. Never let
 * the Nil object be freed!
 *
 * In order to handle I/O errors gracefully, including timeouts, etc... we
 * probably will want to allow NodePtr::ref return an error when resolving
 * pointers from storage.
 *
 * Copy assertion should check if we are copying a null pointer that the
 * physical address is defined.
 */
class NodePtr {
 public:
  NodePtr(SharedNodeRef ref, bool read_only) :
    ref_(ref), csn_(-1), offset_(-1), read_only_(read_only)
  {}

  NodePtr(NodePtr&& other) {
    offset_ = other.offset_;
    csn_ = other.csn_;
    read_only_ = true;

    std::lock_guard<std::mutex> lk(other.lock_);
    ref_ = other.ref_;
  }

  NodePtr(const NodePtr& other) {
    offset_ = other.offset_;
    csn_ = other.csn_;
    read_only_ = true;

    std::lock_guard<std::mutex> lk(other.lock_);
    ref_ = other.ref_;
  }

  NodePtr& operator=(const NodePtr& other) {
    assert(!read_only());
    offset_ = other.offset_;
    csn_ = other.csn_;

    std::lock_guard<std::mutex> lk(other.lock_);
    ref_ = other.ref_;

    return *this;
  }

  NodePtr& operator=(NodePtr&& other) {
    offset_ = other.offset_;
    csn_ = other.csn_;
    read_only_ = true;

    std::lock_guard<std::mutex> lk(other.lock_);
    ref_ = other.ref_;

    return *this;
  }

  inline bool read_only() const {
    return read_only_;
  }

  inline void set_read_only() {
    assert(!read_only());
    read_only_ = true;
  }

  inline SharedNodeRef ref(std::vector<std::pair<int64_t, int>>& trace) {
    trace.emplace_back(csn_, offset_);
    std::unique_lock<std::mutex> lk(lock_);
    if (ref_)
      return ref_;
    lk.unlock();
    auto ptr = fetch(trace);
    assert(ptr);
    lk.lock();
    if (!ref_)
      ref_ = ptr;
    return ref_;
  }

  // deference a node without providing a trace. this is used by the db that
  // doesn't maintain a trace. ideally we want to always (or nearly always)
  // have a trace. this is only for convenience in some routines that do
  // things like print the tree.
  inline SharedNodeRef ref_notrace() {
    std::vector<std::pair<int64_t, int>> trace;
    return ref(trace);
  }

  inline void set_ref(SharedNodeRef ref) {
    assert(!read_only());
    std::lock_guard<std::mutex> lk(lock_);
    ref_ = ref;
  }

  inline int offset() const {
    return offset_;
  }

  inline void set_offset(int offset) {
    assert(!read_only());
    offset_ = offset;
  }

  inline int64_t csn() const {
    return csn_;
  }

  inline void set_csn(int64_t csn) {
    assert(!read_only());
    csn_ = csn;
  }

 private:
  mutable std::mutex lock_;
  SharedNodeRef ref_;

  int64_t csn_;
  int offset_;

  bool read_only_;

  SharedNodeRef fetch(std::vector<std::pair<int64_t, int>>& trace);
};

/*
 * use signed types here and in protobuf so we can see the initial neg values
 */
class Node {
 public:
  NodePtr left;
  NodePtr right;

  // TODO: allow rid to have negative initialization value
  Node(const Slice& key, const Slice& val, bool red, SharedNodeRef lr, SharedNodeRef rr,
      uint64_t rid, bool read_only) :
    left(lr, read_only), right(rr, read_only),
    key_(key.data(), key.size()), val_(val.data(), val.size()),
    red_(red), rid_(rid), read_only_(read_only)
  {}

  static SharedNodeRef& Nil() {
    static SharedNodeRef node = std::make_shared<Node>("", "",
        false, nullptr, nullptr, (uint64_t)-1, true);
    return node;
  }

  SharedNodeRef Copy(uint64_t new_rid) {
    if (this == Nil().get())
      return Nil();

    // TODO: we don't need to use the version of ref() that resolves here
    // because the caller will likely only traverse down one side.
    auto node = std::make_shared<Node>(key_, val_, red_,
        left.ref_notrace(), right.ref_notrace(),
        new_rid, false);

    node->left.set_csn(left.csn());
    node->left.set_offset(left.offset());

    node->right.set_csn(right.csn());
    node->right.set_offset(right.offset());

    return node;
  }

  inline bool read_only() const {
    return read_only_;
  }

  inline void set_read_only() {
    assert(!read_only());
    left.set_read_only();
    right.set_read_only();
    read_only_ = true;
  }

  inline bool red() const {
    return red_;
  }

  inline void set_red(bool red) {
    assert(!read_only());
    red_ = red;
  }

  inline void swap_color(SharedNodeRef other) {
    assert(!read_only());
    assert(!other->read_only());
    std::swap(red_, other->red_);
  }

  inline int64_t rid() const {
    return rid_;
  }

  inline void set_rid(int64_t rid) {
    assert(!read_only());
    assert(rid_ < 0);
    assert(rid >= 0);
    rid_ = rid;
  }

  // TODO: return const reference?
  inline Slice key() const {
    return Slice(key_.data(), key_.size());
  }

  // TODO: return const reference?
  inline Slice val() const {
    return Slice(val_.data(), val_.size());
  }

  inline void steal_payload(SharedNodeRef& other) {
    assert(!read_only());
    assert(!other->read_only());
    key_ = std::move(other->key_);
    val_ = std::move(other->val_);
  }

  size_t ByteSize() {
    return sizeof(*this) + key_.size() + val_.size();
  }

  void Serialize(kvstore_proto::Node& dst) {
    dst.set_red(red_);
    dst.set_key(key_);
    dst.set_val(val_);
  }

 private:
  std::string key_;
  std::string val_;
  bool red_;
  int64_t rid_;
  bool read_only_;
};

#endif
