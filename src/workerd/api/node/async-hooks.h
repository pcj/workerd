// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/async-context.h>
#include <kj/table.h>

namespace workerd::api::node {

class AsyncLocalStorage final: public jsg::Object {
  // Implements a subset of the Node.js AsyncLocalStorage API.
  //
  // Example:
  //
  //   import * as async_hooks from 'node:async_hooks';
  //   const als = new async_hooks.AsyncLocalStorage();
  //
  //   async function doSomethingAsync() {
  //     await scheduler.wait(100);
  //     console.log(als.getStore()); // 1
  //   }
  //
  //   als.run(1, async () => {
  //     console.log(als.getStore());  // 1
  //     await doSomethingAsync();
  //     console.log(als.getStore());  // 1
  //   });
  //   console.log(als.getStore());  // undefined
public:
  AsyncLocalStorage() : key(kj::refcounted<Key>(*this)) {}
  ~AsyncLocalStorage() noexcept(false) { key->reset(); }

  static jsg::Ref<AsyncLocalStorage> constructor();

  v8::Local<v8::Value> run(jsg::Lock& js,
                           v8::Local<v8::Value> store,
                           v8::Local<v8::Function> callback,
                           jsg::Varargs args);

  v8::Local<v8::Value> exit(jsg::Lock& js,
                           v8::Local<v8::Function> callback,
                           jsg::Varargs args);

  v8::Local<v8::Value> getStore(jsg::Lock& js);

  inline void enterWith(jsg::Lock&, v8::Local<v8::Value>) {
    KJ_UNIMPLEMENTED("asyncLocalStorage.enterWith() is not implemented");
  }

  inline void disable(jsg::Lock&) {
    KJ_UNIMPLEMENTED("asyncLocalStorage.disable() is not implemented");
  }

  JSG_RESOURCE_TYPE(AsyncLocalStorage) {
    JSG_METHOD(run);
    JSG_METHOD(exit);
    JSG_METHOD(getStore);
    JSG_METHOD(enterWith);
    JSG_METHOD(disable);
  }

private:
  class Key final: public jsg::AsyncResource::StorageKey {
  public:
    Key(AsyncLocalStorage& ref)
        : ref(ref),
          hash(kj::hashCode("AsyncLocalStorage"_kj, &ref)) {}

    void reset() { ref = nullptr; }
    bool isDead() const override { return ref == nullptr; }
    uint hashCode() const override { return hash; }
  private:
    kj::Maybe<AsyncLocalStorage&> ref;
    uint hash;
  };

  kj::Own<Key> key;
};


class AsyncResource final: public jsg::Object, public jsg::AsyncResource {
  // The AsyncResource API allows applications to define their own Async contexts.
  // For instance, if user code is using custom thenables instead of native promises,
  // or user code wishes to attach event listeners to async contexts.
public:
  struct Options {
    jsg::Optional<uint64_t> triggerAsyncId;

    // Node.js also has an additional `requireManualDestroy` boolean option
    // that we do not implement.

    JSG_STRUCT(triggerAsyncId);
  };

  AsyncResource(jsg::Lock& js, jsg::Optional<kj::String> type, jsg::Optional<Options> options);

  static jsg::Ref<AsyncResource> constructor(jsg::Lock& js, jsg::Optional<kj::String> type,
                                             jsg::Optional<Options> options = nullptr);

  uint64_t asyncId();
  // This resource's id.

  uint64_t triggerAsyncId();
  // The parent resource's id.

  static v8::Local<v8::Function> staticBind(jsg::Lock& js,
                                            v8::Local<v8::Function> fn,
                                            jsg::Optional<kj::String> type,
                                            jsg::Optional<v8::Local<v8::Value>> thisArg,
                                            const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler);
  v8::Local<v8::Function> bind(jsg::Lock& js,
                               v8::Local<v8::Function> fn,
                               jsg::Optional<v8::Local<v8::Value>> thisArg,
                               const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler);
  // Binds the given function to this async context.

  v8::Local<v8::Value> runInAsyncScope(jsg::Lock& js,
                                       v8::Local<v8::Function> fn,
                                       jsg::Optional<v8::Local<v8::Value>> thisArg,
                                       jsg::Varargs args);
  // Calls the given function within this async context.

  JSG_RESOURCE_TYPE(AsyncResource) {
    JSG_STATIC_METHOD_NAMED(bind, staticBind);
    JSG_METHOD(asyncId);
    JSG_METHOD(triggerAsyncId);
    JSG_METHOD(bind);
    JSG_METHOD(runInAsyncScope);
  }

private:
  kj::String type;
  // We currently do not make use of the type. With Node.js' implementation,
  // the type name is reported via the async hook callback apis that we are
  // not implementing.
};

class AsyncHooksModule final: public jsg::Object {
  // We have no intention of fully-implementing the Node.js async_hooks module.
  // We provide this because AsyncLocalStorage is exposed via async_hooks in
  // Node.js.
public:

  uint64_t executionAsyncId(jsg::Lock& js);
  uint64_t triggerAsyncId(jsg::Lock& js);

  JSG_RESOURCE_TYPE(AsyncHooksModule) {
    JSG_NESTED_TYPE(AsyncLocalStorage);
    JSG_NESTED_TYPE(AsyncResource);
    JSG_METHOD(executionAsyncId);
    JSG_METHOD(triggerAsyncId);
  }
};

#define EW_NODE_ASYNCHOOKS_ISOLATE_TYPES       \
    api::node::AsyncHooksModule,               \
    api::node::AsyncResource,                  \
    api::node::AsyncResource::Options,         \
    api::node::AsyncLocalStorage

}  // namespace workerd::api::node
