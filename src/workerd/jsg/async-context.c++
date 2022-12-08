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
      return dynamic_cast<AsyncResource&>(*wrappable);
    }
    return nullptr;
  }
};

}  // namespace

AsyncResource::AsyncResource(
    Lock& js,
    uint64_t id,
    kj::Maybe<AsyncResource&> maybeParent)
    : id(id),
      parentId(maybeParent.map([](auto& parent) {
        return parent.id;
      })) {
  KJ_IF_MAYBE(parent, maybeParent) {
    parent->storage.propagate(js, this->storage);
  }
}

AsyncResource& AsyncResource::current(Lock& js) {
  auto& isolateBase = IsolateBase::from(js.v8Isolate);
  KJ_ASSERT(!isolateBase.asyncResourceStack.empty());
  return *isolateBase.asyncResourceStack.front();
}

kj::Own<AsyncResource> AsyncResource::create(Lock& js, kj::Maybe<AsyncResource&> maybeParent) {
  auto id = IsolateBase::from(js.v8Isolate).getNextAsyncResourceId();
  KJ_IF_MAYBE(parent, maybeParent) {
    KJ_ASSERT(id > parent->id);
    return kj::heap<AsyncResource>(js, id, *parent);
  }
  return kj::heap<AsyncResource>(js, id, current(js));
}

v8::Local<v8::Function> AsyncResource::wrap(
    Lock& js,
    v8::Local<v8::Function> fn,
    kj::Maybe<AsyncResource&> maybeParent) {
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();
  auto handle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "asyncResource"));
  if (!fn->HasPrivate(context, handle).FromJust()) {
    auto id = IsolateBase::from(isolate).getNextAsyncResourceId();
    auto obj = AsyncResourceWrappable::wrap(js, id,
        maybeParent.orDefault(AsyncResource::current(js)));
    KJ_ASSERT(check(fn->SetPrivate(context, handle, obj)));
  }

  return jsg::check(v8::Function::New(context, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = args.GetIsolate();
    auto context = isolate->GetCurrentContext();
    auto fn = args.Data().As<v8::Function>();
    auto handle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "asyncResource"));
    auto& resource = KJ_ASSERT_NONNULL(AsyncResourceWrappable::tryUnwrap(isolate,
        check(fn->GetPrivate(context, handle))));

    AsyncResource::Scope scope(jsg::Lock::from(isolate), resource);
    jsg::check(fn->Call(context, v8::Undefined(isolate), 0, nullptr));
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
  asyncResourceStack.push_front(&next);
}

void IsolateBase::popAsyncResource() {
  asyncResourceStack.pop_front();
  KJ_ASSERT(!asyncResourceStack.empty(), "the async resource stack was corrupted");
}

void IsolateBase::promiseHook(v8::PromiseHookType type,
                              v8::Local<v8::Promise> promise,
                              v8::Local<v8::Value> parent) {
  auto isolate = promise->GetIsolate();
  auto context = isolate->GetCurrentContext();
  auto& isolateBase = IsolateBase::from(isolate);
  auto& js = Lock::from(isolate);

  auto handle = v8::Private::ForApi(isolate, v8StrIntern(isolate, "asyncResource"));

  const auto tryGetAsyncResource = [&](v8::Local<v8::Promise> promise)
      -> kj::Maybe<AsyncResource&> {
    return AsyncResourceWrappable::tryUnwrap(isolate, check(promise->GetPrivate(context, handle)));
  };

  const auto createAsyncResource = [&](v8::Local<v8::Promise> promise, AsyncResource& maybeParent)
        -> AsyncResource& {
    auto id = IsolateBase::from(isolate).getNextAsyncResourceId();
    auto obj = AsyncResourceWrappable::wrap(js, id, maybeParent);
    check(promise->SetPrivate(context, handle, obj));
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
