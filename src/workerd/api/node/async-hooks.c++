#include "async-hooks.h"
#include <kj/vector.h>

namespace workerd::api::node {

jsg::Ref<AsyncLocalStorage> AsyncLocalStorage::constructor() {
  return jsg::alloc<AsyncLocalStorage>();
}

v8::Local<v8::Value> AsyncLocalStorage::run(
    jsg::Lock& js,
    v8::Local<v8::Value> store,
    v8::Local<v8::Function> callback,
    jsg::Varargs args) {
  kj::Vector<v8::Local<v8::Value>> argv(args.size());
  for (auto arg : args) {
    argv.add(arg.getHandle(js));
  }

  auto context = js.v8Isolate->GetCurrentContext();

  jsg::AsyncResource::StorageScope scope(js, *key, js.v8Ref(store));

  return jsg::check(callback->Call(
      context,
      context->Global(),
      argv.size(),
      argv.begin()));
}

v8::Local<v8::Value> AsyncLocalStorage::exit(
    jsg::Lock& js,
    v8::Local<v8::Function> callback,
    jsg::Varargs args) {
  // Node.js defines exit as running "a function synchronously outside of a context".
  // It goes on to say that the store is not accessible within the callback or the
  // asynchronous operations created within the callback. Any getStore() call done
  // within the callbackfunction will always return undefined... except if run() is
  // called which implicitly enables the context again within that scope.
  //
  // We do not have to emulate Node.js enable/disable behavior since we are not
  // implementing the enterWith/disable methods. We can emulate the correct
  // behavior simply by calling run with the store value set to undefined, which
  // will propagate correctly.
  return run(js, v8::Undefined(js.v8Isolate), callback, kj::mv(args));
}

v8::Local<v8::Value> AsyncLocalStorage::getStore(jsg::Lock& js) {
  KJ_IF_MAYBE(value, jsg::AsyncResource::current(js).get(*key)) {
    return value->getHandle(js);
  }
  return v8::Undefined(js.v8Isolate);
}

}  // namespace workerd::api::node
