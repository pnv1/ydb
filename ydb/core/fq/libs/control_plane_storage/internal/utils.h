#pragma once

#include <tuple>

#include <util/string/builder.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <yql/essentials/public/issue/yql_issue_message.h>

#include <ydb/core/fq/libs/config/protos/issue_id.pb.h>
#include <ydb/core/fq/libs/control_plane_storage/proto/yq_internal.pb.h>
#include <ydb/core/fq/libs/protos/fq_private.pb.h>
#include <yql/essentials/utils/exceptions.h>

namespace NFq {

struct TTopicConsumerLess {
    bool operator()(const Fq::Private::TopicConsumer& c1, const Fq::Private::TopicConsumer& c2) const {
        // Cluster endpoint/use ssl are not in key
        return std::tie(c1.database_id(), c1.database(), c1.topic_path(), c1.consumer_name()) < std::tie(c2.database_id(), c2.database(), c2.topic_path(), c2.consumer_name());
    }
};

NYql::TIssues ValidateWriteResultData(const TString& resultId, const Ydb::ResultSet& resultSet, const TInstant& deadline, const TDuration& ttl);

NYql::TIssues ValidateGetTask(const TString& owner, const TString& hostName);

NYql::TIssues ValidatePingTask(const TString& scope, const TString& queryId, const TString& owner, const TInstant& deadline, const TDuration& ttl);

NYql::TIssues ValidateNodesHealthCheck(
    const TString& tenant,
    const TString& instanceId,
    const TString& hostName);

NYql::TIssues ValidateCreateOrDeleteRateLimiterResource(const TString& queryId, const TString& scope, const TString& tenant, const TString& owner);

std::vector<TString> GetMeteringRecords(const TString& statistics, bool billable, const TString& jobId, const TString& scope, const TString& sourceId);

void PackStatisticsToProtobuf(google::protobuf::RepeatedPtrField<FederatedQuery::Internal::StatisticsNamedValue>& dest,
                              const THashMap<TString, i64>& aggregatedStats,
                              TDuration executionTime);
void PackStatisticsToProtobuf(google::protobuf::RepeatedPtrField<FederatedQuery::Internal::StatisticsNamedValue>& dest,
                              std::string_view statsStr,
                              TDuration executionTime);

using TStatsValuesList = std::vector<std::pair<TString, i64>>;

TStatsValuesList ExtractStatisticsFromProtobuf(const google::protobuf::RepeatedPtrField<FederatedQuery::Internal::StatisticsNamedValue>& statsProto);

struct TStatistics {
    operator bool() const noexcept { return !Stats.empty(); }

    const TStatsValuesList& Stats;
};

TStringBuilder& operator<<(TStringBuilder& builder, const TStatistics& statistics);

void AddTransientIssues(::google::protobuf::RepeatedPtrField< ::Ydb::Issue::IssueMessage>* protoIssues, NYql::TIssues&& issues);

};
