/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifdef BRPC_UCX
#include <ucp/api/ucp.h>
#endif
#include <arpa/inet.h>
#include <dlfcn.h>                                // dlopen
#include <fcntl.h>
#include <ifaddrs.h>
#include <pthread.h>
#include <stdlib.h>
#include <vector>
#include <gflags/gflags.h>
#include "butil/atomicops.h"
#include "butil/logging.h"
#include "butil/endpoint.h"
#include "butil/fd_utility.h"                     // butil::make_non_blocking
#include "brpc/socket.h"

namespace brpc {
namespace ucx  {

void* g_handle_ucx = NULL;

// UCX-related functions
#ifdef BRPC_UCX
// Not use static link, no need to solve symbol dynamicly
#endif

static in_addr g_ucx_ip = { 0 };

static butil::atomic<bool> g_ucx_available(false);

#ifdef BRPC_UCX
DEFINE_string(ucx_cluster, "0.0.0.0/0",
              "The ip address prefix of current cluster which supports UCX");

struct UcxCluster {
    uint32_t ip;
    uint32_t mask;
};

static UcxCluster g_cluster = { 0, 0 };

static ucp_context_h g_context = NULL;
static ucp_worker_h  g_worker = NULL;
static SocketId g_async_socket;

static const size_t SYSFS_SIZE = 4096;

static void GlobalRelease() {
    g_ucx_available.store(false, butil::memory_order_release);
    sleep(1);  // to avoid unload library too early

    if (g_worker != NULL) {
	ucp_worker_destroy(g_worker);
	g_worker = NULL;
    }

    if (g_context != NULL) {
        ucp_cleanup(g_context);
        g_context = NULL;
    }

}

static struct UcxCluster ParseUcxCluster(const std::string& str) {
    bool has_error = false;
    struct UcxCluster ucx_cluster;
    ucx_cluster.mask = 0xffffffff;
    ucx_cluster.ip = 0;

    butil::StringPiece ip_str(str);
    size_t pos = str.find('/');
    int len = 32;
    uint32_t ip_addr = 0;
    if (pos != std::string::npos) {
        // Check UCX cluster mask
        butil::StringPiece mask_str(str.c_str() + pos + 1);
        if (mask_str.length() < 1 || mask_str.length() > 2) {
            has_error = true;
        } else {
            char* end = NULL;
            len = strtol(mask_str.data(), &end, 10);
            if (*end != '\0' || len > 32 || len < 0) {
                has_error = true;
            }
        }
        ip_str.remove_suffix(mask_str.length() + 1);
    } else {
        has_error = true;
    }

    if (inet_pton(AF_INET, ip_str.as_string().c_str(), &ip_addr) <= 0) {
        has_error = true;
    } else {
        ip_addr = ntohl(ip_addr);
    }

    if (has_error || len == 0) {
        ucx_cluster.mask = 0;
    } else {
        ucx_cluster.mask <<= 32 - len;
    }

    ucx_cluster.ip = ip_addr & ucx_cluster.mask;
    if (has_error) {
        LOG(WARNING) << "ucx cluster error (" << str
                     << "), the correct configuration should be:"
                     << "ip/mask (0<=mask<=32)";
    }
    return ucx_cluster;
}

static inline void ExitWithError() {
    GlobalRelease();
    exit(1);
}

static void OnUcxWorkerProgress(Socket* m) {
   int progress = Socket::PROGRESS_INIT;
   while (ucp_worker_arm(g_worker) == UCS_ERR_BUSY) {
       ucp_worker_progress(g_worker);
   }
   m->MoreReadEvents(&progress);
}

#endif

static void GlobalUcxInitializeOrDieImpl() {
#ifndef BRPC_UCX
    CHECK(false) << "This libbdrpc.a does not support UCX";
    exit(1);
#else

    ucp_params_t ucp_params;
    ucp_params.field_mask   = UCP_PARAM_FIELD_FEATURES;
    ucp_params.features     = UCP_FEATURE_AM | UCP_FEATURE_WAKEUP;

    ucs_status_t status = ucp_init(&ucp_params, NULL, &g_context);
    if (status != UCS_OK) {
        LOG(ERROR) << "Failed to initialize UCX global resources";
        ExitWithError();
    }

    /* Create worker */
    ucp_worker_params_t worker_params;
    worker_params.field_mask  = UCP_WORKER_PARAM_FIELD_THREAD_MODE;
    worker_params.thread_mode = UCS_THREAD_MODE_MULTI;
    status = ucp_worker_create(g_context, &worker_params, &g_worker);
    if (status != UCS_OK) {
        LOG(ERROR) << "Failed to initialize UCX worker resources";
        ExitWithError();
    }

    SocketOptions opt;
    if (ucp_worker_get_efd(g_worker, &opt.fd) != UCS_OK) {
        LOG(ERROR) << "failed to get ucp_worker fd to be epoll monitored";
        ExitWithError();
    }
    if (butil::make_non_blocking(opt.fd) < 0) {
        PLOG(WARNING) << "Fail to set async_fd to nonblocking";
        ExitWithError();
    }

    while (ucp_worker_arm(g_worker) == UCS_ERR_BUSY) {
	ucp_worker_progress(g_worker);
    }
    opt.on_edge_triggered_events = OnUcxWorkerProgress;
    if (Socket::Create(opt, &g_async_socket) < 0) {
        LOG(WARNING) << "Fail to create socket to deal with UCX event";
        ExitWithError();
    }

    atexit(GlobalRelease);

    g_cluster = ParseUcxCluster(FLAGS_ucx_cluster);
    g_ucx_available.store(true, butil::memory_order_relaxed);
#endif
}

static pthread_once_t initialize_ucx_once = PTHREAD_ONCE_INIT;

void GlobalUcxInitializeOrDie() {
    if (pthread_once(&initialize_ucx_once,
                     GlobalUcxInitializeOrDieImpl) != 0) {
        LOG(FATAL) << "Fail to pthread_once GlobalUcxInitializeOrDie";
        exit(1);
    }
}

bool DestinationInUcxCluster(in_addr_t addr) {
#ifdef BRPC_UCX
    if ((addr & g_cluster.mask) == g_cluster.ip) {
        return true;
    }
#endif
    return false;
}

bool DestinationInGivenCluster(std::string prefix, in_addr_t addr) {
#ifdef BRPC_UCX
    UcxCluster cluster = ParseUcxCluster(prefix);
    if ((addr & cluster.mask) == cluster.ip) {
        return true;
    }
#endif
    return false;
}

in_addr GetUcxIP() {
    return g_ucx_ip;
}

bool IsLocalIP(in_addr addr) {
    butil::ip_t local_ip;
    butil::str2ip("127.0.0.1", &local_ip);
    if (addr.s_addr == butil::ip2int(local_ip) ||
        addr.s_addr == 0 || addr == g_ucx_ip) {
        return true;;
    }
    return false;
}

void* GetUcxContext() {
    // TODO: return ucp_context?
    return NULL;
}

bool IsUcxAvailable() {
    return g_ucx_available.load(butil::memory_order_acquire);
}

void GlobalDisableUcx() {
    if (g_ucx_available.exchange(false, butil::memory_order_acquire)) {
        LOG(FATAL) << "UCX is disabled due to some unrecoverable problem";
    }
}

bool SupportedByUcx(std::string protocol) {
    if (protocol.compare("baidu_std") == 0 ||
        protocol.compare("hulu_pbrpc") == 0 ||
        protocol.compare("sofa_pbrpc") == 0 ||
        protocol.compare("http") == 0) {
        return true;
    }
    return false;
}

}  // namespace ucx
}  // namespace brpc
