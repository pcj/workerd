// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include "jsg.h"
#include <v8.h>

namespace workerd::jsg {

class AsyncResource {
  // Provides for basic internal async context tracking. Eventually, it is expected that
  // this will be provided by V8 assuming that the AsyncContext proposal advances through
  // TC-39. For now, however, we implement a model that is very similar to that implemented
  // by Node.js.
  //
  // The term "resource" here comes from Node.js, which really doesn't take the time to
  // define it properly. Conceptually, an "async resource" is some Thing that generates
  // asynchronous activity over time. For instance, a timer is an async resource that
  // invokes a callback after a certain period of time elapses; a promise is an async
  // resource that may trigger scheduling of a microtask at some point in the future,
  // and so forth. Whether or not "resource" is the best term to use to describe these,
  // it's what we have because our intent here is to stay aligned with Node.js' model
  // as closely as possible.
  //
  // An async resource has an "execution context" or "execution scope". We enter the
  // execution scope immediately before the async resource performs whatever action
  // it is going to perform (e.g. invoking a callback), and exit the execution scope
  // immediately after.
  //
  // Execution scopes form a stack. The default execution scope is the Root (which
  // we label as id = 0). When we enter the execution scope of a different async resource,
  // we push it onto the stack, perform whatever task it is, then pop it back off the
  // stack. The Root is associated with the Isolate itself such that every isolate
  // always has at least one async resource on the stack at all times.
  //
  // Every async resource has a storage context. Whatever async resource is currently
  // at the top of the stack determines the currently active storage context. So, for
  // instance, when we start executing, the Root async resource's storage context is
  // active. When a timeout elapses and a timer is going to fire, we enter the timers
  // execution scope which makes the timers storage context active. Once the timer
  // callback has completed, we return back to the Root async resource's execution
  // scope and storage context.
  //
  // All async resources (except for the Root) are created within the scope of a
  // parent, which by default is whichever async resource is at the top of the stack
  // when the new resource is created.
  //
  // When the new resource is created, it inherits the storage context of the parent.
public:
  const uint64_t id;
  const kj::Maybe<uint64_t> parentId;

  AsyncResource(
      Lock& js,
      uint64_t id,
      kj::Maybe<AsyncResource&> maybeParent = nullptr);

  ~AsyncResource() noexcept(false);

  virtual kj::Maybe<kj::Own<Wrappable>> maybeGetStrongRef() { return nullptr; }

  static AsyncResource& current(Lock& js);

  static kj::Own<AsyncResource> create(Lock& js, kj::Maybe<AsyncResource&> maybeParent = nullptr);
  // Create a new AsyncResource. If maybeParent is not specified, uses the current().

  static v8::Local<v8::Function> wrap(Lock& js, v8::Local<v8::Function> fn,
                                      kj::Maybe<AsyncResource&> maybeParent = nullptr,
                                      kj::Maybe<v8::Local<v8::Value>> thisArg = nullptr);
  // Treats the given JavaScript function as an async resource and returns a wrapper
  // function that will ensure appropriate propagation of the async context tracking
  // when the wrapper function is called.

  struct Scope {
    // AsyncResource::Scope makes the given AsyncResource the current in the
    // stack until the scope is destroyed.
    IsolateBase& isolate;
    Scope(Lock& js, AsyncResource& resource);
    Scope(v8::Isolate* isolate, AsyncResource& resource);
    ~Scope() noexcept(false);
  };

  class StorageKey: public kj::Refcounted {
    // An opaque key that identifies an async-local storage cell within the resource.
  public:
    virtual bool isDead() const = 0;
    virtual uint hashCode() const = 0;
  };

  kj::Maybe<Value&> get(StorageKey& key);

  struct StorageScope {
    // Stores the given value in the current AsyncResource, holding onto, and restoring the
    // previous value when the scope is destroyed.
    AsyncResource& resource;
    StorageKey& key;
    kj::Maybe<Value> oldStore;

    StorageScope(Lock& js, StorageKey& key, Value store);
    ~StorageScope() noexcept(false);
  };

private:
  struct Storage {
    struct Cell {
      kj::Own<StorageKey> key;
      kj::Maybe<Value> value;
    };

    struct CellIndex {
      inline StorageKey& keyForRow(Cell& cell) const { return *cell.key; }
      inline bool matches(Cell& cell, const StorageKey& key) const {
        return cell.key == &key;
      }
      inline uint hashCode(StorageKey& key) const { return key.hashCode(); }
    };

    kj::Table<Cell, kj::HashIndex<CellIndex>, kj::InsertionOrderIndex> cells;

    Storage() : cells(CellIndex(), {}) {}

    kj::Maybe<Value> exchange(StorageKey& key, kj::Maybe<Value>);
    kj::Maybe<Value&> get(StorageKey& key);
    void propagate(Lock& js, Storage& other);
  };

  Storage storage;
  IsolateBase& isolate;

  AsyncResource(IsolateBase& isolate);

  friend struct StorageScope;
  friend class IsolateBase;
};

}  // namespace workerd::jsg
