#pragma once

#include <workerd/jsg/jsg.h>
#include <workerd/jsg/async-context.h>
#include <kj/table.h>

namespace workerd::api::node {

class AsyncLocalStorage final: public jsg::Object {
  // Implements a subset of the Node.js AsyncLocalStorage API.
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

class AsyncResource final: public jsg::Object {
public:
  JSG_RESOURCE_TYPE(AsyncResource) {}
};

class AsyncHooksModule final: public jsg::Object {
  // We have no intention of fully-implementing the Node.js async_hooks module.
  // We provide this because AsyncLocalStorage is exposed via async_hooks in
  // Node.js.
public:
  JSG_RESOURCE_TYPE(AsyncHooksModule) {
    JSG_NESTED_TYPE(AsyncLocalStorage);
    JSG_NESTED_TYPE(AsyncResource);
  }
};

#define EW_NODE_ASYNCHOOKS_ISOLATE_TYPES       \
    api::node::AsyncHooksModule,               \
    api::node::AsyncResource,                  \
    api::node::AsyncLocalStorage

}  // namespace workerd::api::node
