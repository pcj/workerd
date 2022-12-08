// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-context.h"
#include "jsg.h"
#include "setup.h"
#include <v8.h>

namespace workerd::jsg {

namespace {
struct AsyncResourceWrappable final: public Wrappable,
                                     public AsyncResource {
  // Used to attach async context to JS objects like Promises.
  using AsyncResource::AsyncResource;

  static v8::Local<v8::Value> wrap(Lock& js,
                                   uint64_t id,
                                   kj::Maybe<AsyncResource&> maybeParent) {
    auto wrapped = kj::refcounted<AsyncResourceWrappable>(js, id, maybeParent);
    return wrapped->attachOpaqueWrapper(js.v8Isolate->GetCurrentContext(), false);
  }

  static kj::Maybe<AsyncResource&> tryUnwrap(v8::Isolate* isolate,
                                             v8::Local<v8::Value> handle) {
    KJ_IF_MAYBE(wrappable, Wrappable::tryUnwrapOpaque(isolate, handle)) {
      AsyncResource* resource = dynamic_cast<AsyncResource*>(wrappable);
      if (resource != nullptr) {
        return *resource;
      }
    }
    return nullptr;
  }

  kj::Maybe<kj::Own<Wrappable>> maybeGetStrongRef() override {
    return kj::addRef(*this);
  }
};

}  // namespace

AsyncResource::AsyncResource(IsolateBase& isolate)
    : id(0), isolate(isolate) {
  this->isolate.registerAsyncResource(*this);
}

AsyncResource::AsyncResource(
    Lock& js,
    uint64_t id,
    kj::Maybe<AsyncResource&> maybeParent)
    : id(id),
      parentId(maybeParent.map([](auto& parent) {
        return parent.id;
      })),
      isolate(IsolateBase::from(js.v8Isolate)) {
  isolate.registerAsyncResource(*this);
  KJ_IF_MAYBE(parent, maybeParent) {
    parent->storage.propagate(js, this->storage);
  }
}

AsyncResource::~AsyncResource() noexcept(false) {
  isolate.unregisterAsyncResource(*this);
}

AsyncResource& AsyncResource::current(Lock& js) {
  auto& isolateBase = IsolateBase::from(js.v8Isolate);
  KJ_ASSERT(!isolateBase.asyncResourceStack.empty());
  return *isolateBase.asyncResourceStack.front().resource;
}

kj::Own<AsyncResource> AsyncResource::create(Lock& js, kj::Maybe<AsyncResource&> maybeParent) {
  auto id = js.getNextAsyncResourceId();
  KJ_IF_MAYBE(parent, maybeParent) {
    KJ_ASSERT(id > parent->id);
    return kj::heap<AsyncResource>(js, id, *parent);
  }
  return kj::heap<AsyncResource>(js, id, current(js));
}

v8::Local<v8::Function> AsyncResource::wrap(
    Lock& js,
    v8::Local<v8::Function> fn,
    kj::Maybe<AsyncResource&> maybeParent,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();
  auto handle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "asyncResource"));
  if (!fn->HasPrivate(context, handle).FromJust()) {
    auto id = js.getNextAsyncResourceId();
    auto obj = AsyncResourceWrappable::wrap(js, id,
        maybeParent.orDefault(AsyncResource::current(js)));
    KJ_ASSERT(check(fn->SetPrivate(context, handle, obj)));
    KJ_IF_MAYBE(arg, thisArg) {
      auto thisArgHandle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "thisArg"));
      KJ_ASSERT(check(fn->SetPrivate(context, thisArgHandle, *arg)));
    }
  }

  return jsg::check(v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = args.GetIsolate();
    auto context = isolate->GetCurrentContext();
    auto fn = args.Data().As<v8::Function>();
    auto handle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "asyncResource"));
    auto thisArgHandle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "thisArg"));
    auto& resource = KJ_ASSERT_NONNULL(AsyncResourceWrappable::tryUnwrap(isolate,
        check(fn->GetPrivate(context, handle))));

    v8::Local<v8::Value> thisArg = context->Global();
    if (fn->HasPrivate(context, thisArgHandle).FromJust()) {
      thisArg = check(fn->GetPrivate(context, thisArgHandle));
    }

    AsyncResource::Scope scope(jsg::Lock::from(isolate), resource);

    kj::Vector<v8::Local<v8::Value>> argv(args.Length());
    for (int n = 0; n < args.Length(); n++) {
      argv.add(args[n]);
    }

    v8::Local<v8::Value> result;
    if (fn->Call(context, thisArg, args.Length(), argv.begin()).ToLocal(&result)) {
      args.GetReturnValue().Set(result);
    }
  }, fn));
}

kj::Maybe<Value&> AsyncResource::get(StorageKey& key) {
  return storage.get(key);
}

kj::Maybe<Value> AsyncResource::Storage::exchange(StorageKey& key, kj::Maybe<Value> value) {
  cells.eraseAll([](const auto& cell) { return cell.key->isDead(); });
  auto& cell = cells.findOrCreate(key, [key=kj::addRef(key)]() mutable {
    return Cell { kj::mv(key) };
  });
  auto current = kj::mv(cell.value);
  cell.value = kj::mv(value);
  return kj::mv(current);
}

kj::Maybe<Value&> AsyncResource::Storage::get(StorageKey& key) {
  cells.eraseAll([](const auto& cell) { return cell.key->isDead(); });
  return cells.findOrCreate(key, [key=kj::addRef(key)]() mutable {
    return Cell { kj::mv(key) };
  }).value.map([](auto& value) -> Value& { return value; });
}

void AsyncResource::Storage::propagate(Lock& js, Storage& other) {
  if (cells.size() == 0) return;
  for (auto& cell : cells) {
    other.cells.insert(Cell {
      .key = kj::addRef(*cell.key),
      .value = cell.value.map([&js](Value& val) { return val.addRef(js); })
    });
  }
}

AsyncResource::Scope::Scope(Lock& js, AsyncResource& resource)
    : Scope(js.v8Isolate, resource) {}

AsyncResource::Scope::Scope(v8::Isolate* isolate, AsyncResource& resource)
    : isolate(IsolateBase::from(isolate)) {
  this->isolate.pushAsyncResource(resource);
}

AsyncResource::Scope::~Scope() noexcept(false) {
  isolate.popAsyncResource();
}

AsyncResource::StorageScope::StorageScope(
    Lock& js,
    StorageKey& key,
    Value store)
    : resource(AsyncResource::current(js)),
      key(key),
      oldStore(resource.storage.exchange(key, kj::mv(store))) {
  KJ_ASSERT(!key.isDead());
}

AsyncResource::StorageScope::~StorageScope() noexcept(false) {
  auto dropMe = resource.storage.exchange(key, kj::mv(oldStore));
}

void IsolateBase::pushAsyncResource(AsyncResource& next) {
  asyncResourceStack.push_front(AsyncResourceEntry{
    &next,
    next.maybeGetStrongRef()
  });
}

void IsolateBase::popAsyncResource() {
  asyncResourceStack.pop_front();
  KJ_ASSERT(!asyncResourceStack.empty(), "the async resource stack was corrupted");
}

void IsolateBase::promiseHook(v8::PromiseHookType type,
                              v8::Local<v8::Promise> promise,
                              v8::Local<v8::Value> parent) {
  auto isolate = promise->GetIsolate();

  // V8 will call the promise hook even while execution is terminating. In that
  // case we don't want to do anything here.
  if (isolate->IsExecutionTerminating() ||
      isolate->IsDead() ||
      type == v8::PromiseHookType::kResolve) {
    return;
  }

  auto context = isolate->GetCurrentContext();
  auto& isolateBase = IsolateBase::from(isolate);
  auto& js = Lock::from(isolate);

  auto handle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "asyncResource"));

  const auto tryGetAsyncResource = [&](v8::Local<v8::Promise> promise)
      -> kj::Maybe<AsyncResource&> {
    auto obj = check(promise->GetPrivate(context, handle));
    return AsyncResourceWrappable::tryUnwrap(isolate, obj);
  };

  const auto createAsyncResource = [&](v8::Local<v8::Promise> promise, AsyncResource& maybeParent)
        -> AsyncResource& {
    auto id = js.getNextAsyncResourceId();
    auto obj = AsyncResourceWrappable::wrap(js, id, maybeParent);
    KJ_ASSERT(check(promise->SetPrivate(context, handle, obj)));
    return KJ_ASSERT_NONNULL(tryGetAsyncResource(promise));
  };

  const auto trackPromise = [&](
      v8::Local<v8::Promise> promise,
      v8::Local<v8::Value> parent) -> AsyncResource& {
    KJ_IF_MAYBE(asyncResource, tryGetAsyncResource(promise)) {
      return *asyncResource;
    }

    if (parent->IsPromise()) {
      auto parentPromise = parent.As<v8::Promise>();
      KJ_IF_MAYBE(asyncResource, tryGetAsyncResource(parentPromise)) {
        return createAsyncResource(promise, *asyncResource);
      }
      return createAsyncResource(promise,
          createAsyncResource(parentPromise, AsyncResource::current(js)));
    }

    return createAsyncResource(promise, AsyncResource::current(js));
  };

  switch (type) {
    case v8::PromiseHookType::kInit: {
      trackPromise(promise, parent);
      break;
    }
    case v8::PromiseHookType::kBefore: {
      auto& resource = trackPromise(promise, parent);
      isolateBase.pushAsyncResource(resource);
      break;
    }
    case v8::PromiseHookType::kAfter: {
      isolateBase.popAsyncResource();
      break;
    }
    case v8::PromiseHookType::kResolve: {
      // There's nothing to do here.
      break;
    }
  }
}

}  // namespace workerd::jsg
