// Copyright (c) 2014 Baidu, Inc.
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

// A client sending requests to server by multiple threads.

#include <gflags/gflags.h>
#include <bthread/bthread.h>
#include <butil/logging.h>
#include <brpc/server.h>
#include <brpc/channel.h>
#include "echo.pb.h"
#include <bvar/bvar.h>

DEFINE_int32(thread_num, 50, "Number of threads to send requests");
DEFINE_bool(use_bthread, false, "Use bthread to send requests");
DEFINE_int32(attachment_size, 0, "Carry so many byte attachment along with requests");
DEFINE_int32(request_size, 16, "Bytes of each request");
DEFINE_string(protocol, "baidu_std", "Protocol type. Defined in src/brpc/options.proto");
DEFINE_string(connection_type, "", "Connection type. Available values: single, pooled, short");
DEFINE_string(server, "0.0.0.0:8002", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 1000, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)"); 
DEFINE_bool(dont_fail, false, "Print fatal when some call failed");
DEFINE_bool(enable_ssl, false, "Use SSL connection");
DEFINE_int32(dummy_port, -1, "Launch dummy server at this port");
DEFINE_bool(use_ucp, false, "Use ucp connection");

std::string g_request;
std::string g_attachment;

bvar::LatencyRecorder g_latency_recorder("client");
bvar::Adder<int> g_error_count("client_error_count");

std::string server_addr[] = {"10.187.0.91:13339", "10.187.0.6:13339", "10.187.0.41:13339"};

static void* sender(void* arg) {
    constexpr int nch = sizeof(server_addr)/sizeof(server_addr[0]);
    // A Channel represents a communication line to a Server. Notice that 
    // Channel is thread-safe and can be shared by all threads in your program.
    brpc::Channel channel[nch];
    example::EchoService_Stub *stub[nch];
 
    for (int i = 0; i < nch; ++i) {
        // Initialize the channel, NULL means using default options.
        brpc::ChannelOptions options;
        if (FLAGS_enable_ssl) {
            options.mutable_ssl_options();
        }
        options.protocol = FLAGS_protocol;
        options.connection_type = FLAGS_connection_type;
        options.connect_timeout_ms = std::min(FLAGS_timeout_ms / 2, 100);
        options.timeout_ms = FLAGS_timeout_ms;
        options.max_retry = FLAGS_max_retry;
        options.use_ucp = true;
 	if (channel[i].Init(server_addr[i].c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
            LOG(ERROR) << "Fail to initialize channel";
            return NULL;
	}
        stub[i] = new example::EchoService_Stub(static_cast<google::protobuf::RpcChannel*>(&channel[i]));
    }

    int log_id = 0; 
    int which = 0;
    while (!brpc::IsAskedToQuit()) {
        // We will receive response synchronously, safe to put variables
        // on stack.
        example::EchoRequest request;
        example::EchoResponse response;
        brpc::Controller cntl;

        request.set_message(g_request);
        cntl.set_log_id(log_id++);  // set by user
        // Set attachment which is wired to network directly instead of 
        // being serialized into protobuf messages.
        cntl.request_attachment().append(g_attachment);

        // Because `done'(last parameter) is NULL, this function waits until
        // the response comes back or error occurs(including timedout).
        stub[which]->Echo(&cntl, &request, &response, NULL);
        if (!cntl.Failed()) {
            g_latency_recorder << cntl.latency_us();
        } else {
            g_error_count << 1; 
            CHECK(brpc::IsAskedToQuit() || !FLAGS_dont_fail)
                << "error=" << cntl.ErrorText() << " latency=" << cntl.latency_us();
            // We can't connect to the server, sleep a while. Notice that this
            // is a specific sleeping to prevent this thread from spinning too
            // fast. You should continue the business logic in a production 
            // server rather than sleeping.
            bthread_usleep(50000);
        }
	which++;
	if (which >= nch)
	    which = 0;
    }

    for (int i = 0; i < nch; ++i)
	delete stub[i];
    return NULL;
}

int main(int argc, char* argv[]) {
    // Parse gflags. We recommend you to use gflags as well.
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_attachment_size > 0) {
        g_attachment.resize(FLAGS_attachment_size, 'a');
    }
    if (FLAGS_request_size <= 0) {
        LOG(ERROR) << "Bad request_size=" << FLAGS_request_size;
        return -1;
    }
    g_request.resize(FLAGS_request_size, 'r');

    if (FLAGS_dummy_port >= 0) {
        brpc::StartDummyServerAt(FLAGS_dummy_port);
    }

    std::vector<bthread_t> bids;
    std::vector<pthread_t> pids;
    if (!FLAGS_use_bthread) {
        pids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (pthread_create(&pids[i], NULL, sender, NULL) != 0) {
                LOG(ERROR) << "Fail to create pthread";
                return -1;
            }
        }
    } else {
        bids.resize(FLAGS_thread_num);
        for (int i = 0; i < FLAGS_thread_num; ++i) {
            if (bthread_start_background(
                    &bids[i], NULL, sender, NULL) != 0) {
                LOG(ERROR) << "Fail to create bthread";
                return -1;
            }
        }
    }

    while (!brpc::IsAskedToQuit()) {
        sleep(1);
        LOG(INFO) << "Sending EchoRequest at qps=" << g_latency_recorder.qps(1)
                  << " latency=" << g_latency_recorder.latency(1);
    }

    LOG(INFO) << "EchoClient is going to quit";
    for (int i = 0; i < FLAGS_thread_num; ++i) {
        if (!FLAGS_use_bthread) {
            pthread_join(pids[i], NULL);
        } else {
            bthread_join(bids[i], NULL);
        }
    }

    return 0;
}