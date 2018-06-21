/*
 *  Copyright (c) 2018-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <fizz/server/AsyncFizzServer.h>

namespace fizz {
namespace server {

template <typename SM>
AsyncFizzServerT<SM>::AsyncFizzServerT(
    folly::AsyncTransportWrapper::UniquePtr socket,
    const std::shared_ptr<FizzServerContext>& fizzContext,
    const std::shared_ptr<ServerExtensions>& extensions)
    : AsyncFizzBase(std::move(socket)),
      fizzContext_(fizzContext),
      extensions_(extensions),
      visitor_(*this),
      fizzServer_(state_, transportReadBuf_, visitor_, this) {}

template <typename SM>
void AsyncFizzServerT<SM>::accept(HandshakeCallback* callback) {
  handshakeCallback_ = callback;

  fizzServer_.accept(transport_->getEventBase(), fizzContext_, extensions_);
  startTransportReads();
}

template <typename SM>
bool AsyncFizzServerT<SM>::good() const {
  return !error() && transport_->good();
}

template <typename SM>
bool AsyncFizzServerT<SM>::readable() const {
  return transport_->readable();
}

template <typename SM>
bool AsyncFizzServerT<SM>::connecting() const {
  return handshakeCallback_ || transport_->connecting();
}

template <typename SM>
bool AsyncFizzServerT<SM>::error() const {
  return transport_->error() || fizzServer_.inErrorState();
}

template <typename SM>
bool AsyncFizzServerT<SM>::isDetachable() const {
  return !fizzServer_.actionProcessing() && AsyncFizzBase::isDetachable();
}

template <typename SM>
void AsyncFizzServerT<SM>::attachEventBase(folly::EventBase* evb) {
  state_.executor() = evb;
  AsyncFizzBase::attachEventBase(evb);
}

template <typename SM>
folly::ssl::X509UniquePtr AsyncFizzServerT<SM>::getPeerCert() const {
  auto cert = getPeerCertificate();
  if (cert) {
    return cert->getX509();
  } else {
    return nullptr;
  }
}

template <typename SM>
const X509* AsyncFizzServerT<SM>::getSelfCert() const {
  auto cert = getSelfCertificate();
  if (cert) {
    return cert->getX509().get();
  } else {
    return nullptr;
  }
}

template <typename SM>
const Cert* AsyncFizzServerT<SM>::getPeerCertificate() const {
  return getState().clientCert().get();
}

template <typename SM>
const Cert* AsyncFizzServerT<SM>::getSelfCertificate() const {
  return getState().serverCert().get();
}

template <typename SM>
bool AsyncFizzServerT<SM>::isReplaySafe() const {
  // Server always provides replay protection.
  return true;
}

template <typename SM>
void AsyncFizzServerT<SM>::setReplaySafetyCallback(
    folly::AsyncTransport::ReplaySafetyCallback*) {
  LOG(FATAL) << "setReplaySafetyCallback() called on replay safe transport";
}

template <typename SM>
std::string AsyncFizzServerT<SM>::getApplicationProtocol() noexcept {
  if (getState().alpn()) {
    return *getState().alpn();
  } else {
    return "";
  }
}

template <typename SM>
void AsyncFizzServerT<SM>::close() {
  if (transport_->good()) {
    fizzServer_.appClose();
  } else {
    DelayedDestruction::DestructorGuard dg(this);
    folly::AsyncSocketException ase(
        folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
    deliverAllErrors(ase, false);
    transport_->close();
  }
}

template <typename SM>
void AsyncFizzServerT<SM>::closeWithReset() {
  DelayedDestruction::DestructorGuard dg(this);
  if (transport_->good()) {
    fizzServer_.appClose();
  }
  folly::AsyncSocketException ase(
      folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
  deliverAllErrors(ase, false);
  transport_->closeWithReset();
}

template <typename SM>
void AsyncFizzServerT<SM>::closeNow() {
  DelayedDestruction::DestructorGuard dg(this);
  if (transport_->good()) {
    fizzServer_.appClose();
  }
  folly::AsyncSocketException ase(
      folly::AsyncSocketException::END_OF_FILE, "socket closed locally");
  deliverAllErrors(ase, false);
  transport_->closeNow();
}

template <typename SM>
Buf AsyncFizzServerT<SM>::getEkm(
    folly::StringPiece label,
    const Buf& context,
    uint16_t length) const {
  return fizzServer_.getEkm(label, context, length);
}

template <typename SM>
Buf AsyncFizzServerT<SM>::getEarlyEkm(
    folly::StringPiece label,
    const Buf& context,
    uint16_t length) const {
  return fizzServer_.getEarlyEkm(label, context, length);
}

template <typename SM>
void AsyncFizzServerT<SM>::writeAppData(
    folly::AsyncTransportWrapper::WriteCallback* callback,
    std::unique_ptr<folly::IOBuf>&& buf,
    folly::WriteFlags flags) {
  if (error()) {
    if (callback) {
      callback->writeErr(
          0,
          folly::AsyncSocketException(
              folly::AsyncSocketException::INVALID_STATE,
              "fizz app write in error state"));
    }
    return;
  }

  AppWrite write;
  write.callback = callback;
  write.data = std::move(buf);
  write.flags = flags;
  fizzServer_.appWrite(std::move(write));
}

template <typename SM>
void AsyncFizzServerT<SM>::transportError(
    const folly::AsyncSocketException& ex) {
  DelayedDestruction::DestructorGuard dg(this);
  deliverAllErrors(ex);
}

template <typename SM>
void AsyncFizzServerT<SM>::transportDataAvailable() {
  fizzServer_.newTransportData();
}

template <typename SM>
void AsyncFizzServerT<SM>::deliverAllErrors(
    const folly::AsyncSocketException& ex,
    bool closeTransport) {
  deliverHandshakeError(ex);
  fizzServer_.moveToErrorState(ex);
  deliverError(ex, closeTransport);
}

template <typename SM>
void AsyncFizzServerT<SM>::deliverHandshakeError(folly::exception_wrapper ex) {
  if (handshakeCallback_) {
    auto callback = handshakeCallback_;
    handshakeCallback_ = nullptr;
    callback->fizzHandshakeError(this, std::move(ex));
  }
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(DeliverAppData& data) {
  server_.deliverAppData(std::move(data.data));
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(WriteToSocket& data) {
  server_.transport_->writeChain(
      data.callback, std::move(data.data), data.flags);
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(
    ReportEarlyHandshakeSuccess&) {
  if (server_.handshakeCallback_) {
    auto callback = server_.handshakeCallback_;
    server_.handshakeCallback_ = nullptr;
    callback->fizzHandshakeSuccess(&server_);
  }
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(
    ReportHandshakeSuccess&) {
  if (server_.handshakeCallback_) {
    auto callback = server_.handshakeCallback_;
    server_.handshakeCallback_ = nullptr;
    callback->fizzHandshakeSuccess(&server_);
  }
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(ReportError& error) {
  folly::AsyncSocketException ase(
      folly::AsyncSocketException::SSL_ERROR, error.error.what().toStdString());
  server_.deliverHandshakeError(std::move(error.error));
  server_.deliverAllErrors(ase);
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(WaitForData&) {
  server_.fizzServer_.waitForData();

  if (server_.handshakeCallback_) {
    // Make sure that the read callback is installed.
    server_.startTransportReads();
  }
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(MutateState& mutator) {
  mutator(server_.state_);
}

template <typename SM>
void AsyncFizzServerT<SM>::ActionMoveVisitor::operator()(
    AttemptVersionFallback& fallback) {
  if (!server_.handshakeCallback_) {
    VLOG(2) << "fizz fallback without callback";
    return;
  }
  auto callback = server_.handshakeCallback_;
  server_.handshakeCallback_ = nullptr;
  if (!server_.transportReadBuf_.empty()) {
    fallback.clientHello->prependChain(server_.transportReadBuf_.move());
  }
  callback->fizzHandshakeAttemptFallback(std::move(fallback.clientHello));
}
} // namespace server
} // namespace fizz