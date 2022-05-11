/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#ifndef BRPC_UCX_HELPER_H
#define BRPC_UCX_HELPER_H

#include <netinet/in.h>

namespace brpc {
namespace ucx {

// Initialize UCX environment
// Exit if failed
void GlobalUcxInitializeOrDie();

// If the given address is in the same UCX cluster
bool DestinationInUcxCluster(in_addr_t addr);

// Get global UCX context
void* GetUcxContext();

// Get global Ucx IP
in_addr GetUcxIP();

// Whether the give addr is the local address
bool IsLocalIP(in_addr addr);

// If the Ucx environment is available
bool IsUcxAvailable();

// Disable Ucx in the remaining lifetime of the process
void GlobalDisableUcx();

// If the given protocol supported by UCX
bool SupportedByUcx(std::string protocol);

}  // namespace ucx
}  // namespace brpc

#endif // BRPC_UCX_HELPER_H
