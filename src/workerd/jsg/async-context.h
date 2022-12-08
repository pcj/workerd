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
