#include "yql_arrow_push_down.h"

#include <util/generic/vector.h>
#include <ydb/library/yql/providers/generic/pushdown/yql_generic_match_predicate.h>
#include <parquet/statistics.h>

namespace {

TMaybe<NYql::NGenericPushDown::TTimestampColumnStatsData> GetDateStatistics(parquet20::Type::type physicalType, std::shared_ptr<parquet20::Statistics> statistics) {
    switch (physicalType) {
        case parquet20::Type::type::INT32: {
            const parquet20::TypedStatistics<arrow20::Int32Type>* typedStatistics = static_cast<const parquet20::TypedStatistics<arrow20::Int32Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::Days(minValue);
            stats.highValue = TInstant::Days(maxValue);
            return stats;
        }
        case parquet20::Type::type::INT64: {
            const parquet20::TypedStatistics<arrow20::Int64Type>* typedStatistics = static_cast<const parquet20::TypedStatistics<arrow20::Int64Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::Days(minValue);
            stats.highValue = TInstant::Days(maxValue);
            return stats;
        }
        case parquet20::Type::type::BOOLEAN:
        case parquet20::Type::type::INT96:
        case parquet20::Type::type::FLOAT:
        case parquet20::Type::type::DOUBLE:
        case parquet20::Type::type::BYTE_ARRAY:
        case parquet20::Type::type::FIXED_LEN_BYTE_ARRAY:
        case parquet20::Type::type::UNDEFINED:
        return {};
    }
}

TMaybe<NYql::NGenericPushDown::TTimestampColumnStatsData> GetTimestampStatistics(const parquet20::TimestampLogicalType* typestampLogicalType, parquet20::Type::type physicalType, std::shared_ptr<parquet20::Statistics> statistics) {
    int64_t multiplier = 1;
    switch (typestampLogicalType->time_unit()) {
        case parquet20::LogicalType::TimeUnit::unit::UNKNOWN:
        case parquet20::LogicalType::TimeUnit::unit::NANOS:
            return {};
        case parquet20::LogicalType::TimeUnit::unit::MILLIS:
            multiplier *= 1000;
        break;
        case parquet20::LogicalType::TimeUnit::unit::MICROS:
        break;
    }
    switch (physicalType) {
        case parquet20::Type::type::INT32: {
            const parquet20::TypedStatistics<arrow20::Int32Type>* typedStatistics = static_cast<const parquet20::TypedStatistics<arrow20::Int32Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::FromValue(minValue * multiplier);
            stats.highValue = TInstant::FromValue(maxValue * multiplier);
            return stats;
        }
        case parquet20::Type::type::INT64: {
            const parquet20::TypedStatistics<arrow20::Int64Type>* typedStatistics = static_cast<const parquet20::TypedStatistics<arrow20::Int64Type>*>(statistics.get());
            int64_t minValue = typedStatistics->min();
            int64_t maxValue = typedStatistics->max();
            NYql::NGenericPushDown::TTimestampColumnStatsData stats;
            stats.lowValue = TInstant::FromValue(minValue * multiplier);
            stats.highValue = TInstant::FromValue(maxValue * multiplier);
            return stats;
        }
        case parquet20::Type::type::BOOLEAN:
        case parquet20::Type::type::INT96:
        case parquet20::Type::type::FLOAT:
        case parquet20::Type::type::DOUBLE:
        case parquet20::Type::type::BYTE_ARRAY:
        case parquet20::Type::type::FIXED_LEN_BYTE_ARRAY:
        case parquet20::Type::type::UNDEFINED:
        return {};
    }
}

NYql::NGenericPushDown::TColumnStatistics MakeTimestampStatistics(const TString& name, ::Ydb::Type::PrimitiveTypeId type, const TMaybe<NYql::NGenericPushDown::TTimestampColumnStatsData>& statistics) {
    NYql::NGenericPushDown::TColumnStatistics columnStatistics;
    columnStatistics.ColumnName = name;
    columnStatistics.ColumnType.set_type_id(type);
    columnStatistics.Timestamp = statistics;
    return columnStatistics;
}

bool MatchRowGroup(std::unique_ptr<parquet20::RowGroupMetaData> rowGroupMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    TMap<TString, NYql::NGenericPushDown::TColumnStatistics> columns;
    for (int i = 0; i < rowGroupMetadata->schema()->num_columns(); i++) {
        auto columnChunkMetadata = rowGroupMetadata->ColumnChunk(i);
        if (!columnChunkMetadata->is_stats_set()) {
            continue;
        }
        auto column = rowGroupMetadata->schema()->Column(i);
        std::shared_ptr<const parquet20::LogicalType> logicalType = column->logical_type();
        parquet20::Type::type physicalType = column->physical_type();
        switch (logicalType->type()) {
            case parquet20::LogicalType::Type::type::DATE: {
                auto statistics = GetDateStatistics(physicalType, columnChunkMetadata->statistics());
                if (statistics) {
                    const TString columnName{column->name()};
                    columns[columnName] = MakeTimestampStatistics(columnName, ::Ydb::Type::DATE, statistics);
                }
            }
            break;
            case parquet20::LogicalType::Type::type::TIMESTAMP: {
                const parquet20::TimestampLogicalType* typestampLogicalType = static_cast<const parquet20::TimestampLogicalType*>(logicalType.get());
                auto statistics = GetTimestampStatistics(typestampLogicalType, physicalType, columnChunkMetadata->statistics());
                if (statistics) {
                    const TString columnName{column->name()};
                    columns[columnName] = MakeTimestampStatistics(columnName, ::Ydb::Type::TIMESTAMP, statistics);
                }
            }
            break;
            case parquet20::LogicalType::Type::type::UNDEFINED:
            case parquet20::LogicalType::Type::type::STRING:
            case parquet20::LogicalType::Type::type::MAP:
            case parquet20::LogicalType::Type::type::LIST:
            case parquet20::LogicalType::Type::type::ENUM:
            case parquet20::LogicalType::Type::type::DECIMAL:
            case parquet20::LogicalType::Type::type::TIME:
            case parquet20::LogicalType::Type::type::INTERVAL:
            case parquet20::LogicalType::Type::type::INT:
            case parquet20::LogicalType::Type::type::NIL:
            case parquet20::LogicalType::Type::type::JSON:
            case parquet20::LogicalType::Type::type::BSON:
            case parquet20::LogicalType::Type::type::UUID:
            case parquet20::LogicalType::Type::type::NONE:
            case parquet20::LogicalType::Type::type::FLOAT16:
            break;
        }
    }
    return NYql::NGenericPushDown::MatchPredicate(columns, predicate);
}

TVector<ui64> MatchedRowGroupsImpl(parquet20::FileMetaData* fileMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    TVector<ui64> matchedRowGroups;
    matchedRowGroups.reserve(fileMetadata->num_row_groups());
    for (int i = 0; i < fileMetadata->num_row_groups(); i++) {
        if (MatchRowGroup(fileMetadata->RowGroup(i), predicate)) {
            matchedRowGroups.push_back(i);
        }
    }
    return matchedRowGroups;
}

}

namespace NYql::NDq {

TVector<ui64> MatchedRowGroups(std::shared_ptr<parquet20::FileMetaData> fileMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    return MatchedRowGroupsImpl(fileMetadata.get(), predicate);
}

TVector<ui64> MatchedRowGroups(const std::unique_ptr<parquet20::FileMetaData>& fileMetadata, const NYql::NConnector::NApi::TPredicate& predicate) {
    return MatchedRowGroupsImpl(fileMetadata.get(), predicate);
}



} // namespace NYql::NDq
