#pragma once

#include <semaphore>
#include <thread>
#include <functional>

#include <util/thread/pool.h>
#include <ydb/public/lib/json_value/ydb_json_value.h>
#include <ydb/public/lib/ydb_cli/common/command.h>
#include <ydb/public/lib/ydb_cli/common/formats.h>
#include <ydb/public/sdk/cpp/client/ydb_types/status/status.h>
#include <ydb/public/sdk/cpp/client/ydb_types/fluent_settings_helpers.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <util/generic/size_literals.h>

namespace NYdb {

class TDriver;

namespace NOperation {
class TOperationClient;
}
namespace NScheme {
class TSchemeClient;
}
namespace NTable {
class TTableClient;
}
namespace NImport {
class TImportClient;
}

namespace NConsoleClient {

struct TImportFileSettings : public TOperationRequestSettings<TImportFileSettings> {
    using TSelf = TImportFileSettings;

    static constexpr ui64 MaxBytesPerRequest = 8_MB;
    static constexpr const char * DefaultDelimiter = ",";
    static constexpr ui32 MaxRetries = 10000;

    // Allowed values: Csv, Tsv, JsonUnicode, JsonBase64. Default means Csv
    FLUENT_SETTING_DEFAULT(TDuration, OperationTimeout, TDuration::Seconds(5 * 60));
    FLUENT_SETTING_DEFAULT(TDuration, ClientTimeout, OperationTimeout_ + TDuration::Seconds(5));
    FLUENT_SETTING_DEFAULT(EDataFormat, Format, EDataFormat::Default);
    FLUENT_SETTING_DEFAULT(EBinaryStringEncoding, BinaryStringsEncoding, EBinaryStringEncoding::Unicode);
    FLUENT_SETTING_DEFAULT(ui64, BytesPerRequest, 1_MB);
    FLUENT_SETTING_DEFAULT(ui64, FileBufferSize, 48_MB);
    FLUENT_SETTING_DEFAULT(ui64, MaxInFlightRequests, 200);
    FLUENT_SETTING_DEFAULT(ui64, Threads, std::thread::hardware_concurrency());
    // Settings below are for CSV format only
    FLUENT_SETTING_DEFAULT(ui32, SkipRows, 0);
    FLUENT_SETTING_DEFAULT(bool, Header, false);
    FLUENT_SETTING_DEFAULT(bool, NewlineDelimited, false);
    FLUENT_SETTING_DEFAULT(TString, HeaderRow, "");
    FLUENT_SETTING_DEFAULT(TString, Delimiter, DefaultDelimiter);
    FLUENT_SETTING_DEFAULT(std::optional<TString>, NullValue, std::nullopt);
    FLUENT_SETTING_DEFAULT(bool, Verbose, false);
};

class TMaxInflightGetter {
public:
    TMaxInflightGetter(ui64 totalMaxInFlight, std::atomic<ui64>& currentFileCount)
        : TotalMaxInFlight(totalMaxInFlight)
        , CurrentFileCount(currentFileCount) {
    }

    ~TMaxInflightGetter() {
        --CurrentFileCount;
    }

    ui64 GetCurrentMaxInflight() const {
        return (TotalMaxInFlight - 1) / CurrentFileCount + 1; // round up
    }

private:
    ui64 TotalMaxInFlight;
    std::atomic<ui64>& CurrentFileCount;
};

class TFileChunk {
public:
    TFileChunk(TFile file, THolder<IInputStream>&& stream, ui64 size = std::numeric_limits<ui64>::max());

    bool ConsumeLine(TString& line);
    ui64 GetReadCount() const;

private:
    TFile File;
    THolder<IInputStream> Stream;
    THolder<TCountingInput> CountStream;
    ui64 Size;
};

class TCsvFileReader {
public:

public:
    TCsvFileReader(const TString& filePath, const TImportFileSettings& settings, TString& headerRow,
                   TMaxInflightGetter& inFlightGetter);
    TFileChunk& GetChunk(size_t threadId);
    size_t GetThreadLimit(size_t thread_id) const;
    size_t GetSplitCount() const;

private:
    TVector<TFileChunk> Chunks;
    size_t SplitCount;
    size_t MaxInFlight;
};

class TImportFileClient {
public:
    explicit TImportFileClient(const TDriver& driver, const TClientCommand::TConfig& rootConfig);
    TImportFileClient(const TImportFileClient&) = delete;

    // Ingest data from the input files to the database table.
    //   fsPaths: vector of paths to input files
    //   dbPath: full path to the database table, including the database path
    //   settings: input data format and operational settings
    TStatus Import(const TVector<TString>& fsPaths, const TString& dbPath, const TImportFileSettings& settings = {});

private:
    std::shared_ptr<NTable::TTableClient> TableClient;
    std::shared_ptr<NScheme::TSchemeClient> SchemeClient;

    NTable::TBulkUpsertSettings UpsertSettings;
    NTable::TRetryOperationSettings RetrySettings;

    std::unique_ptr<const NTable::TTableDescription> DbTableInfo;

    std::atomic<ui64> CurrentFileCount = 0;
    std::atomic<ui64> TotalBytesRead = 0;
    TInstant StartTime;
    std::atomic<ui64> TotalInflight = 0;
    std::mutex StatsLock;
    TDuration TotalTimes = TDuration::Zero();
    double TotalCpuTimes = 0;
    ui64 TotalBatchBytes = 0;
    ui64 TotalBatches = 0;
    THolder<std::counting_semaphore<>> WorkersSemaphore;
    THolder<std::counting_semaphore<>> InflightSemaphore;
    std::atomic<bool> Failed = 0;
    THolder<TStatus> ErrorStatus;
    std::mutex StatusLock;


    static constexpr ui32 VerboseModeReadSizeStep = 1 << 27; // 128 MB

    using ProgressCallbackFunc = std::function<void (ui64, ui64)>;

    TStatus UpsertCsvSimple(IInputStream& input,
                                     const TString& filePath,
                                     size_t maxInFlightRequests,
                                     ui64 bytesPerRequest,
                                     THolder<IThreadPool>& pool);

    TStatus UpsertCsv(IInputStream& input,
                      const TString& dbPath,
                      const TImportFileSettings& settings,
                      const TString& filePath,
                      std::optional<ui64> inputSizeHint,
                      ProgressCallbackFunc & progressCallback,
                      THolder<IThreadPool>& pool);

    TStatus UpsertCsvBlock(TFileChunk& chunk,
                            const TString& dbPath,
                            const TImportFileSettings& settings,
                            const TString& filePath,
                            const TType& lineType,
                            const TVector<TString>& header,
                            const TString& headerRow,
                            char delimeter,
                            const std::optional<TString>& nullValue,
                            bool removeLastDelimiter,
                            THolder<IThreadPool>& pool);

    TStatus UpsertCsvTest(const TString& filePath,
                            const TString& dbPath,
                            const TImportFileSettings& settings,
                            THolder<IThreadPool>& pool);

    TStatus UpsertCsvByBlocks(const TString& filePath,
                              const TString& dbPath,
                              const TImportFileSettings& settings);

    TAsyncStatus UpsertTValueBuffer(const TString& dbPath, TValueBuilder& builder);

    TAsyncStatus UpsertTValueBuffer(const TString& dbPath, TValue&& rows);

    TStatus UpsertJson(IInputStream &input, const TString &dbPath, const TImportFileSettings &settings,
                    std::optional<ui64> inputSizeHint, ProgressCallbackFunc & progressCallback);

    TStatus UpsertParquet(const TString& filename, const TString& dbPath, const TImportFileSettings& settings,
                    ProgressCallbackFunc & progressCallback);
    TAsyncStatus UpsertParquetBuffer(const TString& dbPath, const TString& buffer, const TString& strSchema);

    TType GetTableType();
    std::map<TString, TType> GetColumnTypes();
    void ValidateTValueUpsertTable();
};

}
}
