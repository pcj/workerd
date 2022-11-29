// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "system-streams.h"
#include "util.h"
#include <kj/one-of.h>
#include <kj/compat/gzip.h>

namespace workerd::api {

// =======================================================================================
// EncodedAsyncInputStream

namespace {

class EncodedAsyncInputStream final: public ReadableStreamSource {
  // A wrapper around a native `kj::AsyncInputStream` which knows the underlying encoding of the
  // stream and whether or not it requires pending event registration.

public:
  explicit EncodedAsyncInputStream(kj::Own<kj::AsyncInputStream> inner, StreamEncoding encoding,
                                   IoContext& context);

  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;
  // Read bytes in identity encoding. If the stream is not already in identity encoding, it will be
  // converted to identity encoding via an appropriate stream wrapper.

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding outEncoding) override;
  // Return the number of bytes, if known, which this input stream will produce if the sink is known
  // to be of a particular encoding.
  //
  // It is likely an error to call this function without immediately following it with a pumpTo()
  // to a EncodedAsyncOutputStream of that exact encoding.

  kj::Maybe<Tee> tryTee(uint64_t limit) override;
  // Consume this stream and return two streams with the same encoding that read the exact same
  // data.
  //
  // This implementation of `tryTee()` is not technically required for correctness, but prevents
  // re-encoding (and converting Content-Length responses to chunk-encoded responses) gzip streams.

private:
  friend class EncodedAsyncOutputStream;

  void ensureIdentityEncoding();

  kj::Own<kj::AsyncInputStream> inner;
  StreamEncoding encoding;

  IoContext& ioContext;
};

EncodedAsyncInputStream::EncodedAsyncInputStream(
    kj::Own<kj::AsyncInputStream> inner, StreamEncoding encoding, IoContext& context)
    : inner(kj::mv(inner)), encoding(encoding), ioContext(context) {}

kj::Promise<size_t> EncodedAsyncInputStream::tryRead(
    void* buffer, size_t minBytes, size_t maxBytes) {
  ensureIdentityEncoding();

  return kj::evalNow([&]() {
    return inner->tryRead(buffer, minBytes, maxBytes)
      .attach(ioContext.registerPendingEvent());
  }).catch_([](kj::Exception&& exception) -> kj::Promise<size_t> {
    KJ_IF_MAYBE(e, translateKjException(exception, {
      { "gzip compressed stream ended prematurely"_kj,
        "Gzip compressed stream ended prematurely."_kj },
      { "gzip decompression failed"_kj,
        "Gzip decompression failed." },
    })) {
      return kj::mv(*e);
    }

    // Let the original exception pass through, since it is likely already a jsg.TypeError.
    return kj::mv(exception);
  });
}

kj::Maybe<uint64_t> EncodedAsyncInputStream::tryGetLength(StreamEncoding outEncoding) {
  if (outEncoding == encoding) {
    return inner->tryGetLength();
  } else {
    // We have no idea what the length will be once encoded/decoded.
    return nullptr;
  }
}

kj::Maybe<ReadableStreamSource::Tee> EncodedAsyncInputStream::tryTee(uint64_t limit) {
  // We tee the stream in its original encoding, because chances are highest that we'll be pumped
  // to sinks that are of the same encoding, and only read in identity encoding no more than once.
  //
  // Additionally, we should propagate the fact that this stream is a native stream to the branches
  // of the tee, so that branches which fall behind their siblings (and thus are reading from the
  // tee buffer) still register pending events correctly.
  auto tee = kj::newTee(kj::mv(inner), limit);

  Tee result;
  result.branches[0] = newSystemStream(newTeeErrorAdapter(kj::mv(tee.branches[0])), encoding);
  result.branches[1] = newSystemStream(newTeeErrorAdapter(kj::mv(tee.branches[1])), encoding);
  return kj::mv(result);
}

void EncodedAsyncInputStream::ensureIdentityEncoding() {
  if (encoding == StreamEncoding::GZIP) {
    inner = kj::heap<kj::GzipAsyncInputStream>(*inner).attach(kj::mv(inner));
    encoding = StreamEncoding::IDENTITY;
  } else {
    // gzip is the only non-identity encoding we currently support.
    KJ_ASSERT(encoding == StreamEncoding::IDENTITY);
  }
}

// =======================================================================================
// EncodedAsyncOutputStream

class EncodedAsyncOutputStream final: public WritableStreamSink {
  // A wrapper around a native `kj::AsyncOutputStream` which knows the underlying encoding of the
  // stream and optimizes pumps from `EncodedAsyncInputStream`.

public:
  explicit EncodedAsyncOutputStream(kj::Own<kj::AsyncOutputStream> inner, StreamEncoding encoding,
                                    IoContext& context);

  kj::Promise<void> write(const void* buffer, size_t size) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;

  kj::Maybe<kj::Promise<DeferredProxy<void>>> tryPumpFrom(
      ReadableStreamSource& input, bool end) override;

  kj::Promise<void> end() override;

  void abort(kj::Exception reason) override;

private:
  void ensureIdentityEncoding();

  kj::AsyncOutputStream& getInner();
  // Unwrap `inner` as a `kj::AsyncOutputStream`.
  //
  // TODO(cleanup): Obviously this is polymorphism. We should be able to do better.

  kj::OneOf<kj::Own<kj::AsyncOutputStream>, kj::Own<kj::GzipAsyncOutputStream>> inner;
  // I use a OneOf here rather than probing with downcasts because end() must be called for
  // correctness rather than for optimization. I "know" this code will never be compiled w/o RTTI,
  // but I'm paranoid.

  StreamEncoding encoding;

  IoContext& ioContext;
};

EncodedAsyncOutputStream::EncodedAsyncOutputStream(
    kj::Own<kj::AsyncOutputStream> inner, StreamEncoding encoding, IoContext& context)
    : inner(kj::mv(inner)), encoding(encoding), ioContext(context) {}

kj::Promise<void> EncodedAsyncOutputStream::write(const void* buffer, size_t size) {
  ensureIdentityEncoding();

  return getInner().write(buffer, size)
      .attach(ioContext.registerPendingEvent());
}

kj::Promise<void> EncodedAsyncOutputStream::write(
    kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  ensureIdentityEncoding();

  return getInner().write(pieces)
      .attach(ioContext.registerPendingEvent());
}

kj::Maybe<kj::Promise<DeferredProxy<void>>> EncodedAsyncOutputStream::tryPumpFrom(
    ReadableStreamSource& input, bool end) {
  KJ_IF_MAYBE(nativeInput, kj::dynamicDowncastIfAvailable<EncodedAsyncInputStream>(input)) {
    // We can avoid putting our inner streams into identity encoding if the input and output both
    // have the same encoding. Since ReadableStreamSource/WritableStreamSink always pump everything
    // (there is no `amount` parameter like in the KJ equivalents), we can assume that we will
    // always stop at a valid endpoint.
    //
    // Note that even if we have to pump in identity encoding, there is no reason to return nullptr.
    // We can still optimize the pump a little by registering only a single pending event rather
    // than falling back to the heavier weight algorithm in ReadableStreamSource, which depends on
    // tryRead() and write() registering their own individual events on every call.
    if (nativeInput->encoding != encoding) {
      ensureIdentityEncoding();
      nativeInput->ensureIdentityEncoding();
    }

    auto promise = nativeInput->inner->pumpTo(getInner()).ignoreResult();
    if (end) {
      KJ_IF_MAYBE(gz, inner.tryGet<kj::Own<kj::GzipAsyncOutputStream>>()) {
        promise = promise.then([&gz = *gz]() { return gz->end(); });
      }
    }

    // Since this is a system stream, the pump task is eligible to be deferred past IoContext
    // lifetime!
    return kj::Promise<DeferredProxy<void>>(DeferredProxy<void> { kj::mv(promise) });
  }

  return nullptr;
}

kj::Promise<void> EncodedAsyncOutputStream::end() {
  kj::Promise<void> promise = kj::READY_NOW;

  KJ_IF_MAYBE(gz, inner.tryGet<kj::Own<kj::GzipAsyncOutputStream>>()) {
    promise = (*gz)->end();
  }

  return promise.attach(ioContext.registerPendingEvent());
}

void EncodedAsyncOutputStream::abort(kj::Exception reason) {
  // TODO(now): Destroy inner?
}

void EncodedAsyncOutputStream::ensureIdentityEncoding() {
  if (encoding == StreamEncoding::GZIP) {
    // This is safe because only a kj::AsyncOutputStream can have non-identity encoding.
    auto& stream = inner.get<kj::Own<kj::AsyncOutputStream>>();

    inner = kj::heap<kj::GzipAsyncOutputStream>(*stream).attach(kj::mv(stream));
    encoding = StreamEncoding::IDENTITY;
  } else {
    // gzip is the only non-identity encoding we currently support.
    KJ_ASSERT(encoding == StreamEncoding::IDENTITY);
  }
}

kj::AsyncOutputStream& EncodedAsyncOutputStream::getInner() {
  KJ_SWITCH_ONEOF(inner) {
    KJ_CASE_ONEOF(stream, kj::Own<kj::AsyncOutputStream>) {
      return *stream;
    }
    KJ_CASE_ONEOF(gz, kj::Own<kj::GzipAsyncOutputStream>) {
      return *gz;
    }
  }

  KJ_UNREACHABLE;
}

}  // namespace

kj::Own<ReadableStreamSource> newSystemStream(
    kj::Own<kj::AsyncInputStream> inner, StreamEncoding encoding, IoContext& context) {
  return kj::heap<EncodedAsyncInputStream>(kj::mv(inner), encoding, context);
}
kj::Own<WritableStreamSink> newSystemStream(
    kj::Own<kj::AsyncOutputStream> inner, StreamEncoding encoding, IoContext& context) {
  return kj::heap<EncodedAsyncOutputStream>(kj::mv(inner), encoding, context);
}


class WrappedAsyncIoStream final :
    public ReadableStreamSource, public WritableStreamSink, public kj::Refcounted {
  // A wrapper around a native `kj::AsyncIoStream` to enable a ReadableStream and WritableStream
  // to be constructed from it.

public:
  explicit WrappedAsyncIoStream(kj::Own<kj::AsyncIoStream> inner, IoContext& context);
  ~WrappedAsyncIoStream();

  // ReadableStreamSource methods:
  kj::Promise<size_t> tryRead(void* buffer, size_t minBytes, size_t maxBytes) override;

  kj::Maybe<uint64_t> tryGetLength(StreamEncoding outEncoding) override;

  kj::Maybe<Tee> tryTee(uint64_t limit) override;

  // WritableStreamSink methods:
  kj::Promise<void> write(const void* buffer, size_t size) override;
  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override;

  kj::Maybe<kj::Promise<DeferredProxy<void>>> tryPumpFrom(
      ReadableStreamSource& input, bool end) override;

  kj::Promise<void> end() override;

  void abort(kj::Exception reason) override;

private:
  kj::Own<kj::AsyncIoStream> inner;

  IoContext& ioContext;
};

WrappedAsyncIoStream::WrappedAsyncIoStream(
    kj::Own<kj::AsyncIoStream> inner, IoContext& context)
    : inner(kj::mv(inner)), ioContext(context) { }

WrappedAsyncIoStream::~WrappedAsyncIoStream() {
  inner->shutdownWrite();
}

kj::Promise<size_t> WrappedAsyncIoStream::tryRead(
    void* buffer, size_t minBytes, size_t maxBytes) {
  return inner->tryRead(buffer, minBytes, maxBytes)
      .attach(ioContext.registerPendingEvent());
}

kj::Maybe<uint64_t> WrappedAsyncIoStream::tryGetLength(StreamEncoding outEncoding) {
  KJ_REQUIRE(outEncoding == StreamEncoding::IDENTITY);
  return inner->tryGetLength();
}

kj::Maybe<ReadableStreamSource::Tee> WrappedAsyncIoStream::tryTee(uint64_t limit) {
  auto tee = kj::newTee(kj::mv(inner), limit);

  Tee result;
  result.branches[0] = newSystemStream(
      newTeeErrorAdapter(kj::mv(tee.branches[0])), StreamEncoding::IDENTITY);
  result.branches[1] = newSystemStream(
      newTeeErrorAdapter(kj::mv(tee.branches[1])), StreamEncoding::IDENTITY);
  return kj::mv(result);
}

kj::Promise<void> WrappedAsyncIoStream::write(const void* buffer, size_t size) {
  return inner->write(buffer, size).attach(ioContext.registerPendingEvent());
}

kj::Promise<void> WrappedAsyncIoStream::write(
    kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) {
  return inner->write(pieces).attach(ioContext.registerPendingEvent());
}

kj::Maybe<kj::Promise<DeferredProxy<void>>> WrappedAsyncIoStream::tryPumpFrom(
    ReadableStreamSource& input, bool end) {
  return input.pumpTo(*this, end);
}

kj::Promise<void> WrappedAsyncIoStream::end() {
  return kj::READY_NOW;
}

void WrappedAsyncIoStream::abort(kj::Exception reason) {
  inner->shutdownWrite();
  inner->abortRead();
}

SystemMultiStream newSystemMultiStream(
    kj::Own<kj::AsyncIoStream> stream, IoContext& context) {
  auto wrapped = kj::refcounted<WrappedAsyncIoStream>(kj::mv(stream), context);
  return {
    .readable = kj::addRef(*wrapped),
    .writable = kj::mv(wrapped)
  };
}

StreamEncoding getContentEncoding(IoContext& context, const kj::HttpHeaders& headers,
                                  Response::BodyEncoding bodyEncoding) {
  if (bodyEncoding == Response::BodyEncoding::MANUAL) {
    return StreamEncoding::IDENTITY;
  }
  KJ_IF_MAYBE(encodingStr, headers.get(context.getHeaderIds().contentEncoding)) {
    if (*encodingStr == "gzip") {
      return StreamEncoding::GZIP;
    }
  }
  return StreamEncoding::IDENTITY;
}

}  // namespace workerd::api
