// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "async-context.h"
#include "jsg.h"
#include "setup.h"
#include <v8.h>

namespace workerd::jsg {

namespace {
kj::Maybe<AsyncContextFrame&> tryUnwrapFrame(v8::Isolate* isolate, v8::Local<v8::Value> handle) {
  // Gets the held AsyncContextFrame from the opaque wrappable but does not consume it.
  KJ_IF_MAYBE(wrappable, Wrappable::tryUnwrapOpaque(isolate, handle)) {
    OpaqueWrappable<kj::Own<AsyncContextFrame>>* holder =
        dynamic_cast<OpaqueWrappable<kj::Own<AsyncContextFrame>>*>(wrappable);
    KJ_ASSERT(holder != nullptr);
    KJ_ASSERT(!holder->movedAway);
    return *holder->value;
  }
  return nullptr;
}

}  // namespace

AsyncContextFrame::AsyncContextFrame(IsolateBase& isolate)
    : isolate(isolate) {}

AsyncContextFrame::AsyncContextFrame(
    Lock& js,
    kj::Maybe<AsyncContextFrame&> maybeParent,
    kj::Maybe<StorageEntry> maybeStorageEntry)
    : AsyncContextFrame(IsolateBase::from(js.v8Isolate)) {

  // Propagate the storage context of the parent frame to this newly created frame.
  const auto propagate = [&](AsyncContextFrame& parent) {
    parent.storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
    for (auto& entry : parent.storage) {
      storage.insert(entry.clone(js));
    }

    KJ_IF_MAYBE(entry, maybeStorageEntry) {
      storage.upsert(kj::mv(*entry), [](StorageEntry& existing, StorageEntry&& row) mutable {
        existing.value = kj::mv(row.value);
      });
    }
  };

  KJ_IF_MAYBE(parent, maybeParent) {
    propagate(*parent);
  } else {
    propagate(current(js));
  }
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::tryUnwrap(
    Lock& js,
    v8::Local<v8::Promise> promise) {
  auto handle = js.getPrivateSymbolFor("asyncResource"_kj);
  // We do not use the normal unwrapOpaque here since that would consume the wrapped
  // value, and we need to be able to unwrap multiple times.
  return tryUnwrapFrame(js.v8Isolate,
      check(promise->GetPrivate(js.v8Isolate->GetCurrentContext(), handle)));
}

kj::Maybe<AsyncContextFrame&> AsyncContextFrame::tryUnwrap(Lock& js, V8Ref<v8::Promise>& promise) {
  return tryUnwrap(js, promise.getHandle(js));
}

AsyncContextFrame& AsyncContextFrame::current(Lock& js) {
  auto& isolateBase = IsolateBase::from(js.v8Isolate);
  KJ_ASSERT(!isolateBase.asyncFrameStack.empty());
  return *isolateBase.asyncFrameStack.front();
}

kj::Own<AsyncContextFrame> AsyncContextFrame::create(
    Lock& js,
    kj::Maybe<AsyncContextFrame&> maybeParent,
    kj::Maybe<StorageEntry> maybeStorageEntry) {
  return kj::refcounted<AsyncContextFrame>(js, maybeParent, kj::mv(maybeStorageEntry));
}

v8::Local<v8::Function> AsyncContextFrame::wrap(
    Lock& js,
    v8::Local<v8::Function> fn,
    kj::Maybe<AsyncContextFrame&> maybeFrame,
    kj::Maybe<v8::Local<v8::Value>> thisArg) {
  auto isolate = js.v8Isolate;
  auto context = isolate->GetCurrentContext();
  auto handle = js.getPrivateSymbolFor("asyncResource"_kj);

  // Let's make sure the given function has not already been wrapped. If it has,
  // we'll explicitly throw an error since wrapping a function more than once is
  // most likely a bug.
  JSG_REQUIRE(!check(fn->HasPrivate(context, handle)), TypeError,
      "This function has already been associated with an async context.");

  // Because we are working directly with JS functions here and not jsg::Function,
  // we do not have the option of using either an internal field or lambda capture.
  // Instead, we create an opaque wrapper holding a ref to the current frame and set
  // it as a private field on the function.
  auto frame = kj::addRef(AsyncContextFrame::current(js));
  KJ_ASSERT(check(fn->SetPrivate(context, handle, wrapOpaque(context, kj::mv(frame)))));
  KJ_IF_MAYBE(arg, thisArg) {
    auto thisArgHandle = js.getPrivateSymbolFor("thisArg"_kj);
    KJ_ASSERT(check(fn->SetPrivate(context, thisArgHandle, *arg)));
  }

  return jsg::check(v8::Function::New(context,
      [](const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto isolate = args.GetIsolate();
    auto context = isolate->GetCurrentContext();
    auto& js = Lock::from(isolate);
    auto fn = args.Data().As<v8::Function>();
    auto handle = js.getPrivateSymbolFor("asyncResource"_kj);
    auto thisArgHandle = js.getPrivateSymbolFor("thisArg"_kj);

    // We do not use the normal unwrapOpaque here since that would consume the wrapped
    // value, and we need to be able to unwrap multiple times.
    auto& frame = KJ_ASSERT_NONNULL(tryUnwrapFrame(isolate,
        check(fn->GetPrivate(context, handle))));

    v8::Local<v8::Value> thisArg = context->Global();
    if (fn->HasPrivate(context, thisArgHandle).FromJust()) {
      thisArg = check(fn->GetPrivate(context, thisArgHandle));
    }

    AsyncContextFrame::Scope scope(js, frame);

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

v8::Local<v8::Promise> AsyncContextFrame::wrap(
    Lock& js,
    v8::Local<v8::Promise> promise,
    kj::Maybe<AsyncContextFrame&> maybeFrame) {
  auto handle = js.getPrivateSymbolFor("asyncResource"_kj);
  auto context = js.v8Isolate->GetCurrentContext();
  // If the promise has already been wrapped, do nothing else and just return the promise.
  if (!check(promise->HasPrivate(context, handle))) {
    // Otherwise, we have to create an opaque wrapper holding a ref to the current frame
    // because we do not have the option of using an internal field with promises.
    auto frame = kj::addRef(AsyncContextFrame::current(js));
    KJ_ASSERT(check(promise->SetPrivate(context, handle, wrapOpaque(context, kj::mv(frame)))));
  }
  return promise;
}

kj::Maybe<Value&> AsyncContextFrame::get(StorageKey& key) {
  KJ_ASSERT(!key.isDead());
  storage.eraseAll([](const auto& entry) { return entry.key->isDead(); });
  return storage.find(key).map([](auto& entry) -> Value& { return entry.value; });
}

AsyncContextFrame::Scope::Scope(Lock& js, kj::Maybe<AsyncContextFrame&> resource)
    : Scope(js.v8Isolate, resource) {}

AsyncContextFrame::Scope::Scope(v8::Isolate* isolate, kj::Maybe<AsyncContextFrame&> frame)
    : isolate(IsolateBase::from(isolate)) {
  KJ_IF_MAYBE(f, frame) {
    this->isolate.pushAsyncFrame(*f);
  } else {
    this->isolate.pushAsyncFrame(*this->isolate.rootAsyncFrame);
  }
}

AsyncContextFrame::Scope::~Scope() noexcept(false) {
  isolate.popAsyncFrame();
}

AsyncContextFrame::StorageScope::StorageScope(
    Lock& js,
    StorageKey& key,
    Value store)
    : frame(AsyncContextFrame::create(js, nullptr, StorageEntry {
        .key = kj::addRef(key),
        .value = kj::mv(store)
      })),
      scope(js, *frame) {}

bool AsyncContextFrame::isRoot(Lock& js) const {
  return IsolateBase::from(js.v8Isolate).rootAsyncFrame == this;
}

void IsolateBase::pushAsyncFrame(AsyncContextFrame& next) {
  asyncFrameStack.push_front(&next);
}

void IsolateBase::popAsyncFrame() {
  asyncFrameStack.pop_front();
  KJ_DASSERT(!asyncFrameStack.empty(), "the async context frame stack was corrupted");
}

void IsolateBase::promiseHook(v8::PromiseHookType type,
                              v8::Local<v8::Promise> promise,
                              v8::Local<v8::Value> parent) {
  auto isolate = promise->GetIsolate();

  // V8 will call the promise hook even while execution is terminating. In that
  // case we don't want to do anything here.
  if (isolate->IsExecutionTerminating() || isolate->IsDead()) {
    return;
  }

  auto& js = Lock::from(isolate);
  auto& isolateBase = IsolateBase::from(isolate);
  auto& currentFrame = AsyncContextFrame::current(js);

  const auto isRejected = [&] { return promise->State() == v8::Promise::PromiseState::kRejected; };

  js.tryCatch([&] {
    switch (type) {
      case v8::PromiseHookType::kInit: {
        // The kInit event is triggered by v8 when a deferred Promise is created. This
        // includes all calls to `new Promise(...)`, `then()`, `catch()`, `finally()`,
        // uses of `await ...`, `Promise.all()`, etc.
        // Whenever a Promise is created, we associate it with the current AsyncContextFrame.
        // As a performance optimization, we only attach the context if the current is not
        // the root.
        if (!currentFrame.isRoot(js)) {
          KJ_DASSERT(AsyncContextFrame::tryUnwrap(js, promise) == nullptr);
          AsyncContextFrame::wrap(js, promise);
        }
        break;
      }
      case v8::PromiseHookType::kBefore: {
        // The kBefore event is triggered immediately before a Promise continuation.
        // We use it here to enter the AsyncContextFrame that was associated with the
        // promise when it was created.
        KJ_IF_MAYBE(frame, AsyncContextFrame::tryUnwrap(js, promise)) {
          isolateBase.pushAsyncFrame(*frame);
        } else {
          // If the promise does not have a frame attached, we assume the root
          // frame is used. Just to keep bookkeeping easier, we still go ahead
          // and push the frame onto the stack again so we can just unconditionally
          // pop it off in the kAfter without performing additional checks.
          isolateBase.pushAsyncFrame(*isolateBase.rootAsyncFrame);
        }
        // We do not use AsyncContextFrame::Scope here because we do not exit the frame
        // until the kAfter event fires.
        break;
      }
      case v8::PromiseHookType::kAfter: {
  #ifdef KJ_DEBUG
        KJ_IF_MAYBE(frame, AsyncContextFrame::tryUnwrap(js, promise)) {
          // The frame associated with the promise must be the current frame.
          KJ_ASSERT(frame == &currentFrame);
        } else {
          KJ_ASSERT(currentFrame.isRoot(js));
        }
  #endif
        isolateBase.popAsyncFrame();

        // If the promise has been rejected here, we have to maintain the association of the
        // async context to the promise so that the context can be propagated to the unhandled
        // rejection handler. However, if the promise has been fulfilled, we do not expect
        // the context to be used any longer so we can break the context association here and
        // allow the opaque wrapper to be garbage collected.
        if (!isRejected()) {
          auto handle = js.getPrivateSymbolFor("asyncResource"_kj);
          check(promise->DeletePrivate(js.v8Isolate->GetCurrentContext(), handle));
        }

        break;
      }
      case v8::PromiseHookType::kResolve: {
        // This case is a bit different. As an optimization, it appears that v8 will skip
        // the kInit, kBefore, and kAfter events for Promises that are immediately resolved (e.g.
        // Promise.resolve, and Promise.reject) and instead will emit the kResolve event first.
        // When this event occurs, and the promise is rejected, we need to check to see if the
        // promise is already wrapped, and if it is not, do so.
        if (!currentFrame.isRoot(js) && isRejected() &&
            AsyncContextFrame::tryUnwrap(js, promise) == nullptr) {
          AsyncContextFrame::wrap(js, promise);
        }
        break;
      }
    }
  }, [isolate](Value&& exception) {
    isolate->ThrowException(exception.getHandle(isolate));
  });
}

}  // namespace workerd::jsg
