/*
 *
 *    Copyright (c) 2025 Project CHIP Authors
 *    All rights reserved.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <app/clusters/tls-client-management-server/TLSClientManagementCluster.h>
#include <app/storage/FabricTableImpl.h>
#include <vector>

namespace chip {
namespace app {
namespace Clusters {

/// @brief struct used to identify a TLS Endpoint
inline constexpr uint16_t kUndefinedTlsEndpointId = 0xffff;
static constexpr uint8_t kMaxProvisionedEndpoints = 254;

struct TlsEndpointId
{
    uint16_t mEndpointId = kUndefinedTlsEndpointId;

    TlsEndpointId() = default;
    TlsEndpointId(uint16_t id) : mEndpointId(id) {}

    uint16_t & Value() { return mEndpointId; }
    const uint16_t & Value() const { return mEndpointId; }

    void Clear() { mEndpointId = kUndefinedTlsEndpointId; }

    bool IsValid() { return (mEndpointId != kUndefinedTlsEndpointId); }

    bool operator==(const TlsEndpointId & other) const { return (mEndpointId == other.mEndpointId); }
};

class EndpointTable : public app::Storage::FabricTableImpl<TlsEndpointId, TLSClientManagementDelegate::EndpointStructType>
{
public:
    using Super = app::Storage::FabricTableImpl<TlsEndpointId, TLSClientManagementDelegate::EndpointStructType>;

    EndpointTable() : Super(kMaxProvisionedEndpoints, UINT16_MAX) {}
    ~EndpointTable() { Finish(); };
};

/**
 * The application delegate to define the options & implement commands.
 */
class TlsClientManagementCommandDelegate : public TLSClientManagementDelegate
{
    struct Provisioned
    {
        FabricIndex fabric;
        EndpointStructType payload;
    };

    static TlsClientManagementCommandDelegate instance;
    EndpointTable mProvisioned;
    PersistentStorageDelegate * mStorage = nullptr;

    CHIP_ERROR GetEndpointId(FabricIndex fabric, uint16_t & id);

public:
    TlsClientManagementCommandDelegate() {}
    ~TlsClientManagementCommandDelegate() = default;

    CHIP_ERROR Init(PersistentStorageDelegate & storage) override;

    CHIP_ERROR ForEachEndpoint(EndpointId matterEndpoint, FabricIndex fabric, LoadedEndpointCallback callback) override;

    Protocols::InteractionModel::ClusterStatusCode
    ProvisionEndpoint(EndpointId matterEndpoint, FabricIndex fabric,
                      const TlsClientManagement::Commands::ProvisionEndpoint::DecodableType & provisionReq,
                      uint16_t & endpointID) override;

    CHIP_ERROR FindProvisionedEndpointByID(EndpointId matterEndpoint, FabricIndex fabric, uint16_t endpointID,
                                           LoadedEndpointCallback callback) override;

    Protocols::InteractionModel::Status RemoveProvisionedEndpointByID(EndpointId matterEndpoint, FabricIndex fabric,
                                                                      uint16_t endpointID) override;

    CHIP_ERROR RootCertCanBeRemoved(EndpointId matterEndpoint, FabricIndex fabric, Tls::TLSCAID id) override;
    CHIP_ERROR ClientCertCanBeRemoved(EndpointId matterEndpoint, FabricIndex fabric, Tls::TLSCCDID id) override;

    void RemoveFabric(FabricIndex fabric) override;

    CHIP_ERROR MutateEndpointReferenceCount(EndpointId matterEndpoint, FabricIndex fabric, uint16_t endpointID,
                                            int8_t delta) override;

    static inline TlsClientManagementCommandDelegate & GetInstance() { return instance; }
};

/**
 * Initialize the TLS Client Management cluster with application-specific delegate and certificate table.
 * MUST be called before server initialization (e.g. in ApplicationInit()).
 */
void InitializeTlsClientManagement();

/**
 * Serve the TLS Client Management cluster on a DYNAMIC endpoint (e.g. a bridged camera).
 * The ZAP/CodegenIntegration path only instantiates the cluster on the fixed ZAP endpoint;
 * a bridge that disables that endpoint must register per-endpoint instances itself so a
 * controller can provision TLS endpoints for Push AV upload. Also whitelists `endpointId`
 * in the shared delegate (which otherwise only accepts the fixed ZAP endpoint).
 */
void AddTlsClientManagementEndpoint(chip::EndpointId endpointId);

/** Unregister + destroy the dynamic-endpoint instance created by AddTlsClientManagementEndpoint. */
void RemoveTlsClientManagementEndpoint(chip::EndpointId endpointId);

} // namespace Clusters
} // namespace app
} // namespace chip
