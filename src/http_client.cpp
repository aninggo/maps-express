#include "http_client.h"

#include <functional>

#include <folly/Baton.h>

#include <glog/logging.h>

#include <proxygen/lib/http/HTTPConnector.h>
#include <proxygen/lib/http/session/HTTPTransaction.h>
#include <proxygen/lib/http/session/HTTPUpstreamSession.h>

#include "session_wrapper.h"

// TODO: handle task cancellation

static const uint16_t kMaxReconnects = 3u;

using folly::HHWheelTimer;
using proxygen::HTTPMessage;
using proxygen::HTTPConnector;
using proxygen::HTTPUpstreamSession;

struct RequestInfo {
    HTTPClient::http_task_ptr async_task;
    HTTPMessage request;
    std::unique_ptr<folly::IOBuf> request_body;
    std::unique_ptr<proxygen::HTTPMessage> response;
    std::unique_ptr<folly::IOBuf> response_body;
};

class HTTPWorker : public proxygen::HTTPConnector::Callback,
                   public proxygen::HTTPTransactionHandler {

public:
    HTTPWorker(folly::EventBase& evb, folly::HHWheelTimer& timer,
               std::deque<std::unique_ptr<RequestInfo>>& pending_requests,
               const std::string& host, uint16_t port);

    ~HTTPWorker();

    bool Request(std::unique_ptr<RequestInfo> request_info);

    void CancelWork();

    void connectSuccess(proxygen::HTTPUpstreamSession* session) override;
    void connectError(const folly::AsyncSocketException& ex) override;

    void setTransaction(proxygen::HTTPTransaction* txn) noexcept override {}
    void detachTransaction() noexcept override;
    void onHeadersComplete(std::unique_ptr<proxygen::HTTPMessage> msg) noexcept override;
    void onBody(std::unique_ptr<folly::IOBuf> chain) noexcept override;
    void onTrailers(std::unique_ptr<proxygen::HTTPHeaders> trailers) noexcept override {}
    void onEOM() noexcept override;
    void onUpgrade(proxygen::UpgradeProtocol protocol) noexcept override {}
    void onError(const proxygen::HTTPException& error) noexcept override;
    void onEgressPaused() noexcept override;
    void onEgressResumed() noexcept override;

    inline bool busy() const noexcept {
        return request_info_ != nullptr;
    }

    inline bool connected() const noexcept {
        return bool(session_);
    }

private:
    void Connect();
    bool ResolveHostname();
    void SendRequest(HTTPUpstreamSession* session);
    bool MaybeProcessNextRequest();
    bool MaybeResetHostPort(const std::string& host, uint16_t port);

    HTTPConnector connector_;
    folly::SocketAddress addr_;
    folly::EventBase& evb_;
    folly::HHWheelTimer& timer_;
    std::deque<std::unique_ptr<RequestInfo>>& pending_requests_;
    std::string host_;
    uint16_t port_;

    std::unique_ptr<RequestInfo> request_info_;
    SessionWrapper session_;
    proxygen::HTTPTransaction* txn_{nullptr};
    uint16_t num_reconnects_{0};

    bool hostname_resolved_{false};
    bool hold_connection_{false};
};


HTTPWorker::HTTPWorker(folly::EventBase& evb, folly::HHWheelTimer& timer,
                       std::deque<std::unique_ptr<RequestInfo>>& pending_requests,
                       const std::string& host, uint16_t port) :
    connector_(this, &timer),
    evb_(evb),
    timer_(timer),
    pending_requests_(pending_requests),
    host_(host),
    port_(port),
    hostname_resolved_(false)
{
    if (!host.empty()) {
        ResolveHostname();
    }
    MaybeProcessNextRequest();
}

HTTPWorker::~HTTPWorker() {
    if (txn_) {
        txn_->sendAbort();
        txn_ = nullptr;
    }
    connector_.reset();
}

void HTTPWorker::Connect() {
    connector_.reset();
    connector_.connect(&evb_, addr_, std::chrono::milliseconds(3000));
}

bool HTTPWorker::ResolveHostname() {
    try {
        // Note, this does a synchronous DNS lookup
        addr_.setFromHostPort(host_, port_);
    } catch (const std::system_error& e) {
        LOG(ERROR) << "Failed to resolve hostname \"" << host_ << "\": " << e.what();
        return false;
    }
    hostname_resolved_ = true;
    return true;
}


void HTTPWorker::SendRequest(HTTPUpstreamSession* session) {
    assert(request_info_);
    txn_ = session->newTransaction(this);
    txn_->sendHeaders(request_info_->request);
    if (request_info_->request_body) {
        txn_->sendBody(std::move(request_info_->request_body));
    }
    txn_->sendEOM();
}

bool HTTPWorker::MaybeProcessNextRequest() {
    if (!pending_requests_.empty()) {
        auto request_info = std::move(pending_requests_.front());
        pending_requests_.pop_front();
        return Request(std::move(request_info));
    }
    return false;
}

inline bool HTTPWorker::MaybeResetHostPort(const std::string& host, uint16_t port) {
    bool reset = false;
    if (host_ != host) {
        host_ = host;
        reset = true;
    }
    if (port_ != port) {
        port_ = port;
        reset = true;
    }
    if (reset) {
        hostname_resolved_ = false;
    }
    return reset;
}


bool HTTPWorker::Request(std::unique_ptr<RequestInfo> request_info) {
    if (request_info_) {
        LOG(ERROR) << "HTTPWorker already processing request";
        request_info->async_task->NotifyError(HTTPClient::Error::internal);
        return false;
    }
    proxygen::URL url(request_info->request.getURL());
    MaybeResetHostPort(url.getHost(), url.getPort());
    if (!hostname_resolved_ && !ResolveHostname()) {
        request_info->async_task->NotifyError(HTTPClient::Error::resolution);
        return false;
    }
    request_info_ = std::move(request_info);
    if (session_ && !session_->isClosing()) {
        SendRequest(session_.get());
    } else {
        Connect();
    }
    return true;
}


void HTTPWorker::connectSuccess(HTTPUpstreamSession* session) {
    num_reconnects_ = 0;
    if (request_info_) {
        SendRequest(session);
    }
    session->closeWhenIdle();
}

void HTTPWorker::connectError(const folly::AsyncSocketException& ex) {
    LOG(ERROR) << ex.what();
    if (num_reconnects_ < kMaxReconnects) {
        ++num_reconnects_;
        Connect();
        return;
    }
    if (request_info_) {
        request_info_->async_task->NotifyError(HTTPClient::Error::connection);
    }
    num_reconnects_ = 0;
    request_info_.reset();
    MaybeProcessNextRequest();
}

void HTTPWorker::detachTransaction() noexcept {
    txn_ = nullptr;
}

void HTTPWorker::onHeadersComplete(std::unique_ptr<proxygen::HTTPMessage> msg) noexcept {
    assert(request_info_);
    request_info_->response = std::move(msg);
}

void HTTPWorker::onBody(std::unique_ptr<folly::IOBuf> chain) noexcept {
    assert(request_info_);
    if (request_info_->response_body) {
        request_info_->response_body->prependChain(std::move(chain));
    } else {
        request_info_->response_body = std::move(chain);
    }
}

void HTTPWorker::onEOM() noexcept {
    assert(request_info_);
    auto http_response = std::make_shared<HTTPResponse>(HTTPResponse{std::move(request_info_->response),
                                                                     std::move(request_info_->response_body)});
    request_info_->async_task->SetResult(http_response);
    request_info_.reset();
    MaybeProcessNextRequest();
}

void HTTPWorker::onError(const proxygen::HTTPException& error) noexcept {
    if (request_info_) {
        if (error.getProxygenError() == proxygen::kErrorTimeout) {
            request_info_->async_task->NotifyError(HTTPClient::Error::timeout);
        } else {
            LOG(WARNING) << error;
            request_info_->async_task->NotifyError(HTTPClient::Error::network);
        }
        request_info_.reset();
    }
    MaybeProcessNextRequest();
}

void HTTPWorker::onEgressPaused() noexcept {

}

void HTTPWorker::onEgressResumed() noexcept {

}


HTTPClient::HTTPClient(folly::EventBase& evb, const std::string& host, uint16_t port, uint8_t num_workers) :
    evb_(evb)
{
    evb_.runImmediatelyOrRunInEventBaseThreadAndWait([&, host, port]{
        timer_ = HHWheelTimer::newTimer(
                    &evb,
                    std::chrono::milliseconds(HHWheelTimer::DEFAULT_TICK_INTERVAL),
                    folly::AsyncTimeout::InternalEnum::NORMAL,
                    std::chrono::milliseconds(50000));

        for (uint8_t i = 0; i < num_workers; ++i) {
            workers_pool_.push_back(std::make_unique<HTTPWorker>(evb_, *timer_, pending_requests_,
                                                                 host, port));
        }
    });
}

HTTPClient::~HTTPClient() {
    Shutdown();
}

void HTTPClient::Shutdown() {
    if (stopped_) {
        return;
    }
    evb_.runImmediatelyOrRunInEventBaseThreadAndWait([this]{
        workers_pool_.clear();
        for (auto& request_info : pending_requests_) {
            request_info->async_task->NotifyError(Error::shutdown);
        }
        stopped_ = true;
    });
}


void HTTPClient::Request(http_task_ptr async_task, proxygen::HTTPMethod method, const std::string& url,
                         const proxygen::HTTPHeaders* headers, std::unique_ptr<folly::IOBuf> body) {
    if (stopped_) {
        async_task->NotifyError(Error::shutdown);
        return;
    }
    auto request_info = std::make_unique<RequestInfo>();
    request_info->async_task = std::move(async_task);
    HTTPMessage& request= request_info->request;
    request.setMethod(method);
    request.setURL(url);
    proxygen::HTTPHeaders& req_headers = request.getHeaders();
    if (headers) {
        headers->copyTo(req_headers);
    }
    if (body) {
        req_headers.rawAdd("Content-Length", std::to_string(body->computeChainDataLength()));
        request_info->request_body = std::move(body);
    }

    // TODO: maybe avoid bloking
    evb_.runImmediatelyOrRunInEventBaseThreadAndWait([this, request_info = std::move(request_info)]() mutable {
        for (auto& wrk : workers_pool_) {
            if (!wrk->busy()) {
                wrk->Request(std::move(request_info));
                return;
            }
        }
        pending_requests_.push_back(std::move(request_info));
    });
}

http_response_ptr HTTPClient::RequestAndWait(proxygen::HTTPMethod method, const std::string& url,
                                             const proxygen::HTTPHeaders* headers,
                                             std::unique_ptr<folly::IOBuf> body) {
    if (stopped_) {
        return nullptr;
    }
    if (evb_.isInEventBaseThread()) {
        LOG(ERROR) << "HTTPClient::RequestAndWait called from HTTPClient's thread!";
        return nullptr;
    }

    folly::Baton<> baton;
    http_response_ptr response;
    auto task = std::make_shared<HTTPTask>([&](http_response_ptr r) {
        response = std::move(r);
        baton.post();
    }, [&](HTTPClient::Error err) {
        baton.post();
    }, false);
    Request(std::move(task), method, url, headers, std::move(body));
    baton.wait();
    return response;
}

