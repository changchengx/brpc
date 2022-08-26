// Copyright (c) 2022 Netease, Inc.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: Xu Yifeng

#include "brpc/ucp_connection.h"
#include "brpc/ucp_worker.h"
#include "brpc/ucp_cm.h"
#include "brpc/socket.h"
#include "bvar/bvar.h"

#include <stdlib.h>
#include <string.h>
#include <gflags/gflags.h>
#include <sys/socket.h>
#include <ucs/datastruct/list.h>


namespace brpc {

DEFINE_int32(brpc_ucp_ping_timeout, 10, "Number of seconds");

bthread_attr_t ucp_consumer_thread_attr = BTHREAD_ATTR_NORMAL;
static bvar::Adder<int> g_ucp_conn("ucp_connection_count");

UcpAmMsg::UcpAmMsg()
{
    header.cmd = UCP_CMD_BRPC;
    header.pad = 0;
    header.sn = -1;
    data = nullptr;
    length = 0;
    nvec = 0;
    code = UCS_OK;
    req = nullptr;
    flags = 0;
}

UcpAmMsg *UcpAmMsg::Allocate(void)
{
    butil::ResourceId<UcpAmMsg> id;
    auto o = butil::get_resource<UcpAmMsg>(&id);
    o->id = id;
    return o;
}

void UcpAmMsg::Release(UcpAmMsg *o)
{
    o->conn.reset();
    o->header.cmd = UCP_CMD_BRPC;
    o->header.pad = 0;
    o->header.sn = -1;
    o->data = nullptr;
    o->length = 0;
    o->nvec = 0;
    o->code = UCS_OK;
    o->req = nullptr;
    o->flags = 0;
    o->buf.clear();
    butil::return_resource<UcpAmMsg>(o->id);
}

UcpAmSendInfo::UcpAmSendInfo()
{
    header.sn = -1;
    code = UCS_OK;
    req = nullptr;
    nvec = 0;
}

UcpAmSendInfo *UcpAmSendInfo::Allocate(void)
{
    butil::ResourceId<UcpAmSendInfo> id;
    auto o = butil::get_resource<UcpAmSendInfo>(&id);
    o->id = id;
    return o;
}

void UcpAmSendInfo::Release(UcpAmSendInfo *o)
{
    o->conn.reset();
    o->header.sn = -1;
    o->code = UCS_OK;
    o->req = nullptr;
    o->nvec = 0;
    o->buf.clear();
    butil::return_resource<UcpAmSendInfo>(o->id);
}

UcpConnection::UcpConnection(UcpCm *cm, UcpWorker *w)
    : cm_(cm)
    , worker_(w)
    , ep_(NULL)
    , ucp_code_(UCS_OK)
    , ucp_recv_code_(UCS_OK)
    , socket_id_(-1)
    , socket_id_set_(false)
    , state_(STATE_NONE)
    , data_ready_flag_(false)
    , expect_sn_(0)
    , ready_list_(NULL)
{
    ping_seq_ = 0;
    bthread_rwlock_init(&mutex_, NULL);
    remote_side_.set_ucp();
    TAILQ_INIT(&recv_q_);
    TAILQ_INIT(&send_q_);
    next_send_sn_ = 0;
    g_ucp_conn << 1;
}

UcpConnection::~UcpConnection()
{
    if (ep_) {
        LOG(ERROR) << "ep_ should be NULL";
    }
    bthread_rwlock_destroy(&mutex_);
    DLOG(INFO) << __func__ << " " << this;
    g_ucp_conn << -1;
}

void *UcpConnection::operator new(size_t size)
{
    return aligned_alloc(BAIDU_CACHELINE_SIZE, size);
}

void UcpConnection::operator delete(void *ptr)
{
    free(ptr);
}

int UcpConnection::Accept(ucp_conn_request_h req)
{
    bthread::v2::wlock_guard g(mutex_);

    int rc = worker_->Accept(this, req);
    if (rc == 0) {
        state_ = STATE_OPEN;
    }
    return rc;
}

int UcpConnection::Connect(const butil::EndPoint &peer)
{
    bthread::v2::wlock_guard g(mutex_);

    int rc = worker_->Connect(this, peer);
    if (rc == 0) {
        state_ = STATE_OPEN;
    }
    return rc;
}

void UcpConnection::Close()
{
    bthread::v2::wlock_guard g(mutex_);

    if (state_ != STATE_OPEN) {
        return;
    }
    worker_->Release(this);
    WakePing();
}

SocketId UcpConnection::GetSocketId() const
{
    return socket_id_;
}  

void UcpConnection::WakePing()
{
    std::unique_lock<bthread::Mutex> lg(ping_mutex_);
    ping_cond_.notify_all();
}

void UcpConnection::SetSocketId(SocketId id)
{
    bthread::v2::wlock_guard g(mutex_);

    socket_id_ = id;
    socket_id_set_ = true;
    if (ucp_code_.load()) {
        worker_->SetDataReady(this);
    } else if (state_ != STATE_CLOSED) {
        worker_->StartRecv(this);
    }
}

// called by UcpWorker to notify brpc Socket when data is ready
void UcpConnection::DataReady()
{
    data_ready_flag_ = false;
    if (state_ == STATE_OPEN && socket_id_set_)
        Socket::StartInputEvent(socket_id_, EPOLLIN, ucp_consumer_thread_attr);
    else {
        if (state_ != STATE_OPEN) {
            DLOG(WARNING) << "brpc::Socket is closed, "
                             "DataReady does not notify the socket";
        } else {
            DLOG(WARNING) << "brpc::Socket id is not set, "
                             "DataReady does not notify the socket";
        }
    }
    if (ucp_code_.load(butil::memory_order_relaxed) ||
        ucp_recv_code_.load(butil::memory_order_relaxed)) {
        WakePing();
    }
}

ssize_t UcpConnection::Read(butil::IOBuf *out, size_t size_hint)
{
    ssize_t rc;
    bthread::v2::rlock_guard g(mutex_);
 
    // Connection closed, return EOF
    if (state_ != STATE_OPEN) {
        LOG(ERROR) << "Read with closed state";
        return 0;
    }

    worker_->MergeInputMessage(this);

    rc = in_buf_.cutn(out, size_hint); 

    if (rc == 0) {
        if (ucp_code_.load()) // IO error happened, return EOF
            return 0;
    }

    if (rc == 0) {
        rc = -1;
        errno = EAGAIN;
    }
 
    return rc;
}

ssize_t UcpConnection::Write(butil::IOBuf *buf)
{
    butil::IOBuf *data_list[1];
    data_list[0] = buf;

    return Write(data_list, 1);
}

ssize_t UcpConnection::Write(butil::IOBuf *data_list[], int ndata)
{
    bthread::v2::rlock_guard g(mutex_);

    if (state_ != STATE_OPEN) {
        errno = ENOTCONN;
        return -1;
    }

    if (ucp_code_.load()) {
        errno = ECONNRESET;
        return -1;
    }

    ssize_t len = worker_->StartSend(UCP_CMD_BRPC, this, data_list, ndata);
    if (ucp_code_.load()) {
        errno = ECONNRESET;
        return -1;
    }
    return len;
}

int UcpConnection::Ping(const timespec* abstime)
{
    int rc = DoPing(abstime);
    if (rc) {
        int err = errno;
        LOG(ERROR) << "Ping " << remote_side_str_ << " (" << strerror(err) << ")";
        errno = err;
    }
    return rc;
}

int UcpConnection::DoPing(const timespec* abstime)
{
    butil::IOBuf buf;
    struct timespec ts;

    if (abstime == NULL) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += FLAGS_brpc_ucp_ping_timeout;
        abstime = &ts;
    }
    bthread::v2::rlock_guard lg(mutex_);

    if (state_ != STATE_OPEN) {
        errno = ENOTCONN;
        return -1;
    }

    if (ucp_code_.load()) {
        errno = ECONNRESET;
        return -1;
    }

    buf.append("ping", 5);

    ping_mutex_.lock();
    int old = ping_seq_;
    ping_mutex_.unlock();

    ssize_t rc = worker_->StartSend(UCP_CMD_PING, this, &buf);
    if (rc == -1) {
        return -1;
    }

    lg.unlock();

    rc = 0;
    std::unique_lock<bthread::Mutex> pg(ping_mutex_);
    while (rc == 0 && old == ping_seq_ && state_ != STATE_CLOSED &&
           ucp_code_.load() == 0 && ucp_recv_code_.load() == 0) {
        rc = ping_cond_.wait_until(pg, *abstime);
    }
    if (rc == 0) {
        if (state_ == STATE_CLOSED)
            rc = ENOTCONN;
        else if (ucp_code_.load() || ucp_recv_code_.load())
            rc = ECONNRESET;
    }
    errno = rc;
    return rc ? -1 : 0;
}

void UcpConnection::HandlePong(UcpAmMsg *msg)
{
    std::unique_lock<bthread::Mutex> lg(ping_mutex_);
    ping_seq_++;
    ping_cond_.notify_all();
    UcpAmMsg::Release(msg);
}

} // namespace brpc