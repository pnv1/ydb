#include <ydb/library/yql/providers/s3/actors/yql_arrow_push_down.h>

#include <library/cpp/testing/unittest/registar.h>

#include <contrib/libs/apache/arrow_next/cpp/src/parquet/arrow/schema.h>

#include <contrib/libs/apache/arrow_next/cpp/src/parquet/statistics.h>

#include <google/protobuf/text_format.h>

namespace NYql::NPathGenerator {

struct TFileMetaDataBuilder {
    struct TRowGroupBuilder {
        TRowGroupBuilder(TFileMetaDataBuilder* parent,
                         std::shared_ptr<parquet20::SchemaDescriptor> schema,
                         parquet20::RowGroupMetaDataBuilder* rowGroup)
            : Parent(parent)
            , Schema(schema)
            , RowGroup(rowGroup)
        {}

        TRowGroupBuilder& AddColumnTimestampStatistics(int64_t columnId, const int64_t min, const int64_t max) {
            auto columnChunk = RowGroup->NextColumnChunk();
            auto stat = parquet20::MakeStatistics<parquet20::Int64Type>(Schema->Column(columnId));
            stat->SetMinMax(min, max);
            columnChunk->SetStatistics(stat->Encode());
            return *this;
        }

        TFileMetaDataBuilder& Build() {
            return *Parent;
        }

    private:
        TFileMetaDataBuilder* Parent;
        std::shared_ptr<parquet20::SchemaDescriptor> Schema;
        parquet20::RowGroupMetaDataBuilder* RowGroup;
    };

    TFileMetaDataBuilder(const TVector<std::shared_ptr<arrow20::Field>>& columns) {
        auto schema = arrow20::schema(columns);
        parquet20::WriterProperties::Builder builder;
        auto properties = builder.build();
        
        UNIT_ASSERT(parquet20::arrow20::ToParquetSchema(schema.get(), *properties, &Schema) == ::arrow20::Status::OK());

       FileMetadata = parquet20::FileMetaDataBuilder::Make(Schema.get(), properties);
    }

    TRowGroupBuilder AddRowGroup() {
        return TRowGroupBuilder(this, Schema, FileMetadata->AppendRowGroup());
    }

    std::unique_ptr<parquet20::FileMetaData> Build() {
        return FileMetadata->Finish();
    }

private:
    std::unique_ptr<parquet20::FileMetaDataBuilder> FileMetadata;
    std::shared_ptr<parquet20::SchemaDescriptor> Schema;
};

NYql::NConnector::NApi::TPredicate BuildPredicate(const TString& text) {
    NYql::NConnector::NApi::TPredicate predicate;
    UNIT_ASSERT(google::protobuf::TextFormat::ParseFromString(text, &predicate));
    return predicate;
}

Y_UNIT_TEST_SUITE(TArrowPushDown) {
    Y_UNIT_TEST(SimplePushDown) {
        TFileMetaDataBuilder builder{{
            arrow20::field("field1", arrow20::timestamp(arrow20::TimeUnit::type::MILLI)),
            arrow20::field("field2", arrow20::int64()),
            arrow20::field("field3", arrow20::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-03-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds())
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value {
                            column: "field1"
                        }
                        right_value {
                            typed_value {
                                type {
                                    type_id: TIMESTAMP
                                }
                                value {
                                    int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                }
                            }
                        }
                    }
                )proto");

        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 1);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 0);
    }

    Y_UNIT_TEST(FilterEverything) {
        TFileMetaDataBuilder builder{{
            arrow20::field("field1", arrow20::timestamp(arrow20::TimeUnit::type::MILLI)),
            arrow20::field("field2", arrow20::int64()),
            arrow20::field("field3", arrow20::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-13T00:00:00Z").MilliSeconds())
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value {
                            column: "field1"
                        }
                        right_value {
                            typed_value {
                                type {
                                    type_id: TIMESTAMP
                                }
                                value {
                                    int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                }
                            }
                        }
                    }
                )proto");

        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 0);
    }

    Y_UNIT_TEST(MatchSeveralRowGroups) {
        TFileMetaDataBuilder builder{{
            arrow20::field("field1", arrow20::timestamp(arrow20::TimeUnit::type::MILLI)),
            arrow20::field("field2", arrow20::int64()),
            arrow20::field("field3", arrow20::float64())
        }};
        auto fileMetadata = builder.AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-03-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds())
                                   .Build()
                                   .AddRowGroup()
                                   .AddColumnTimestampStatistics(0, TInstant::ParseIso8601("2024-02-01T00:00:00Z").MilliSeconds(), TInstant::ParseIso8601("2024-04-01T00:00:00Z").MilliSeconds())
                                   .Build()
                            .Build();

        auto predicate = BuildPredicate(
                        R"proto(
                    comparison {
                        operation: L
                        left_value {
                            column: "field1"
                        }
                        right_value {
                            typed_value {
                                type {
                                    type_id: TIMESTAMP
                                }
                                value {
                                    int64_value: 1709290801000000 # 2024-03-01T11:00:01.000Z
                                }
                            }
                        }
                    }
                )proto");

        auto rowGroups = NDq::MatchedRowGroups(fileMetadata, predicate);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups.size(), 2);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[0], 0);
        UNIT_ASSERT_VALUES_EQUAL(rowGroups[1], 1);
    }
}

}
