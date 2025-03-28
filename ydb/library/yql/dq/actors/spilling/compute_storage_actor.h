#pragma once

#include "spilling_counters.h"

#include "ydb/library/yql/dq/common/dq_common.h"

#include <ydb/library/actors/core/actor.h>

#include <yql/essentials/utils/chunked_buffer.h>

#include <library/cpp/threading/future/future.h>

namespace NYql::NDq {

class IDqComputeStorageActor
{
public:
    using TPtr = std::shared_ptr<IDqComputeStorageActor>;
    using TKey = ui64;

    virtual ~IDqComputeStorageActor() = default;

    virtual NActors::IActor* GetActor() = 0;
};

struct TDqComputeStorageActorEvents {
    enum {
        EvPut = EventSpaceBegin(NActors::TEvents::EEventSpace::ES_USERSPACE) + 30000,
        EvGet,
        EvExtract,
        EvDelete
    };
};

struct TEvPut : NActors::TEventLocal<TEvPut, TDqComputeStorageActorEvents::EvPut> {
    TEvPut(TChunkedBuffer&& blob, NThreading::TPromise<IDqComputeStorageActor::TKey>&& promise)
        : Blob_(std::move(blob))
        , Promise_(std::move(promise))
    {
    }

    TChunkedBuffer Blob_;
    NThreading::TPromise<IDqComputeStorageActor::TKey> Promise_;
};

struct TEvGet : NActors::TEventLocal<TEvGet, TDqComputeStorageActorEvents::EvGet> {
    TEvGet(IDqComputeStorageActor::TKey key, NThreading::TPromise<std::optional<TChunkedBuffer>>&& promise, bool removeBlobAfterRead)
        : Key_(key)
        , Promise_(std::move(promise))
        , RemoveBlobAfterRead_(removeBlobAfterRead)
    {
    }

    IDqComputeStorageActor::TKey Key_;
    NThreading::TPromise<std::optional<TChunkedBuffer>> Promise_;
    bool RemoveBlobAfterRead_;
};

struct TEvDelete : NActors::TEventLocal<TEvDelete, TDqComputeStorageActorEvents::EvDelete> {
    TEvDelete(IDqComputeStorageActor::TKey key, NThreading::TPromise<void>&& promise)
        : Key_(key)
        , Promise_(std::move(promise))
    {
    }

    IDqComputeStorageActor::TKey Key_;
    NThreading::TPromise<void> Promise_;
};

IDqComputeStorageActor* CreateDqComputeStorageActor(TTxId txId, const TString& spillerName, TWakeUpCallback wakeupCallback, 
    TErrorCallback errorCallback, TIntrusivePtr<TSpillingTaskCounters> spillingTaskCounters);

} // namespace NYql::NDq
