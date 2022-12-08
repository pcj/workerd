// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
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

namespace {
kj::Maybe<jsg::AsyncResource&> getParent(
    jsg::Lock& js,
    jsg::Optional<AsyncResource::Options>& maybeOptions) {
  KJ_IF_MAYBE(options, maybeOptions) {
    return js.tryGetAsyncResource(options->triggerAsyncId.orDefault(0));
  }
  return jsg::AsyncResource::current(js);
}
}  // namespace

AsyncResource::AsyncResource(jsg::Lock& js,
                             jsg::Optional<kj::String> type,
                             jsg::Optional<Options> options)
    : jsg::AsyncResource(js, js.getNextAsyncResourceId(), getParent(js, options)),
      type(kj::mv(type).orDefault([] { return kj::str("AsyncResource"); })) {}

jsg::Ref<AsyncResource> AsyncResource::constructor(jsg::Lock& js,
                                                   jsg::Optional<kj::String> type,
                                                   jsg::Optional<Options> options) {
  return jsg::alloc<AsyncResource>(js, kj::mv(type), kj::mv(options));
}

uint64_t AsyncResource::asyncId() { return id; }
uint64_t AsyncResource::triggerAsyncId() { return parentId.orDefault(0); }

v8::Local<v8::Function> AsyncResource::staticBind(
    jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<kj::String> type,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler) {
  return AsyncResource::constructor(js, kj::mv(type)
      .orDefault([] { return kj::str("AsyncResource"); }))
          ->bind(js, fn, thisArg, handler);
}

v8::Local<v8::Function> AsyncResource::bind(
    jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    const jsg::TypeHandler<jsg::Ref<AsyncResource>>& handler) {
  v8::Local<v8::Function> bound = jsg::AsyncResource::wrap(js, fn, *this, thisArg);
  // Serves the same purpose as attach() in KJ things. Ensures that we hold a reference
  // to the AsyncResource object wrapper for as long as the function is held.
  jsg::check(bound->SetPrivate(js.v8Isolate->GetCurrentContext(),
             v8::Private::ForApi(js.v8Isolate, jsg::v8StrIntern(js.v8Isolate, "ref")),
             handler.wrap(js, JSG_THIS)));
  return bound;
}

v8::Local<v8::Value> AsyncResource::runInAsyncScope(
    jsg::Lock& js,
    v8::Local<v8::Function> fn,
    jsg::Optional<v8::Local<v8::Value>> thisArg,
    jsg::Varargs args) {
  kj::Vector<v8::Local<v8::Value>> argv(args.size());
  for (auto arg : args) {
    argv.add(arg.getHandle(js));
  }

  auto context = js.v8Isolate->GetCurrentContext();

  jsg::AsyncResource::Scope scope(js, *this);

  return jsg::check(fn->Call(
      context,
      thisArg.orDefault(context->Global()),
      argv.size(),
      argv.begin()));
}

uint64_t AsyncHooksModule::executionAsyncId(jsg::Lock& js) {
  return jsg::AsyncResource::current(js).id;
}

uint64_t AsyncHooksModule::triggerAsyncId(jsg::Lock& js) {
  return jsg::AsyncResource::current(js).parentId.orDefault(0);
}

}  // namespace workerd::api::node
