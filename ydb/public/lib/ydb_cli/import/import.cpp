#include "import.h"

#include <ydb/public/sdk/cpp/client/ydb_driver/driver.h>
#include <ydb/public/sdk/cpp/client/ydb_operation/operation.h>
#include <ydb/public/sdk/cpp/client/ydb_proto/accessor.h>
#include <ydb/public/sdk/cpp/client/ydb_scheme/scheme.h>
#include <ydb/public/sdk/cpp/client/ydb_table/table.h>

#include <ydb/public/api/protos/ydb_formats.pb.h>
#include <ydb/public/api/protos/ydb_table.pb.h>
#include <ydb/public/lib/json_value/ydb_json_value.h>
#include <ydb/public/lib/ydb_cli/common/csv_parser.h>
#include <ydb/public/lib/ydb_cli/common/recursive_list.h>
#include <ydb/public/lib/ydb_cli/common/interactive.h>
#include <ydb/public/lib/ydb_cli/common/progress_bar.h>
#include <ydb/public/lib/ydb_cli/dump/util/util.h>
#include <ydb/public/lib/ydb_cli/import/cli_arrow_helpers.h>

#include <library/cpp/string_utils/csv/csv.h>
#include <library/cpp/threading/future/async.h>

#include <util/folder/path.h>
#include <util/generic/vector.h>
#include <util/stream/file.h>
#include <util/stream/length.h>
#include <util/string/builder.h>
#include <util/system/thread.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/io/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/ipc/api.h>
#include <contrib/libs/apache/arrow/cpp/src/arrow/result.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/reader.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/arrow/reader.h>
#include <contrib/libs/apache/arrow/cpp/src/parquet/file_reader.h>

#include <stack>

#if defined(_win32_)
#include <windows.h>
#include <io.h>
#elif defined(_unix_)
#include <unistd.h>
#endif


namespace NYdb {
namespace NConsoleClient {
namespace {

inline
TStatus MakeStatus(EStatus code = EStatus::SUCCESS, const TString& error = {}) {
    NYql::TIssues issues;
    if (error) {
        issues.AddIssue(NYql::TIssue(error));
    }
    return TStatus(code, std::move(issues));
}

TStatus WaitForQueue(const size_t maxQueueSize, std::vector<TAsyncStatus>& inFlightRequests) {
    while (!inFlightRequests.empty() && inFlightRequests.size() >= maxQueueSize) {
        NThreading::WaitAny(inFlightRequests).Wait();
        ui32 delta = 0;
        for (ui32 i = 0; i + delta < inFlightRequests.size();) {
            if (inFlightRequests[i].HasValue() || inFlightRequests[i].HasException()) {
                auto status = inFlightRequests[i].ExtractValueSync();
                if (!status.IsSuccess()) {
                    return status;
                }
                ++delta;
                inFlightRequests[i] = inFlightRequests[inFlightRequests.size() - delta];
            } else {
                ++i;
            }
        }
        inFlightRequests.resize(inFlightRequests.size() - delta);
    }

    return MakeStatus();
}

void InitCsvParser(TCsvParser& parser,
                   bool& removeLastDelimiter,
                   NCsvFormat::TLinesSplitter& csvSource,
                   const TImportFileSettings& settings,
                   const std::map<TString, TType>* columnTypes,
                   const NTable::TTableDescription* dbTableInfo) {
    if (settings.Header_ || settings.HeaderRow_) {
        TString headerRow;
        if (settings.Header_) {
            headerRow = csvSource.ConsumeLine();
        }
        if (settings.HeaderRow_) {
            headerRow = settings.HeaderRow_;
        }
        if (headerRow.EndsWith("\r\n")) {
            headerRow.erase(headerRow.size() - 2);
        }
        if (headerRow.EndsWith("\n")) {
            headerRow.erase(headerRow.size() - 1);
        }
        if (headerRow.EndsWith(settings.Delimiter_)) {
            removeLastDelimiter = true;
            headerRow.erase(headerRow.size() - settings.Delimiter_.size());
        }
        parser = TCsvParser(std::move(headerRow), settings.Delimiter_[0], settings.NullValue_, columnTypes);
        return;
    }

    TVector<TString> columns;
    Y_ENSURE_BT(dbTableInfo);
    for (const auto& column : dbTableInfo->GetColumns()) {
        columns.push_back(column.Name);
    }
    parser = TCsvParser(std::move(columns), settings.Delimiter_[0], settings.NullValue_, columnTypes);
    return;
}

FHANDLE GetStdinFileno() {
#if defined(_win32_)
    return GetStdHandle(STD_INPUT_HANDLE);
#elif defined(_unix_)
    return STDIN_FILENO;
#endif
}

} // namespace

TCsvFileReader::TCsvFileReader(const TString& filePath, const TImportFileSettings& settings, TString& headerRow,
                   TMaxInflightGetter& inFlightGetter) {
    TFile file;
    if (filePath) {
        file = TFile(filePath, RdOnly);
    } else {
        file = TFile(GetStdinFileno());
    }
    auto input = MakeHolder<TFileInput>(file);
    TCountingInput countInput(input.Get());

    if (settings.Header_) {
        headerRow = NCsvFormat::TLinesSplitter(countInput).ConsumeLine();
    }
    for (ui32 i = 0; i < settings.SkipRows_; ++i) {
        NCsvFormat::TLinesSplitter(countInput).ConsumeLine();
    }
    i64 skipSize = countInput.Counter();

    MaxInFlight = inFlightGetter.GetCurrentMaxInflight();
    i64 fileSize = file.GetLength();
    if (filePath.empty() || fileSize == -1) {
        SplitCount = 1;
        Chunks.emplace_back(file, std::move(input));
        return;
    }

    SplitCount = Min(settings.Threads_, MaxInFlight, (fileSize - skipSize) / settings.BytesPerRequest_ + 1);
    i64 chunkSize = (fileSize - skipSize) / SplitCount;
    if (chunkSize == 0) {
        SplitCount = 1;
        chunkSize = fileSize - skipSize;
    }

    i64 curPos = skipSize;
    i64 seekPos = skipSize;
    Chunks.reserve(SplitCount);
    TString temp;
    file = TFile(filePath, RdOnly);
    file.Seek(seekPos, sSet);
    THolder<TFileInput> stream = MakeHolder<TFileInput>(file);
    for (size_t i = 0; i < SplitCount; ++i) {
        seekPos += chunkSize;
        i64 nextPos = seekPos;
        auto nextFile = TFile(filePath, RdOnly);
        auto nextStream = MakeHolder<TFileInput>(nextFile);
        nextFile.Seek(seekPos, sSet);
        if (seekPos > 0) {
            nextFile.Seek(-1, sCur);
            nextPos += nextStream->ReadLine(temp);
        }

        Chunks.emplace_back(file, std::move(stream), nextPos - curPos);
        file = std::move(nextFile);
        stream = std::move(nextStream);
        curPos = nextPos;
    }
}

TFileChunk& TCsvFileReader::GetChunk(size_t threadId) {
    if (threadId >= Chunks.size()) {
        throw yexception() << "File chunk number is too big";
    }
    return Chunks[threadId];
}

size_t TCsvFileReader::GetThreadLimit(size_t thread_id) const {
    return MaxInFlight / SplitCount + (thread_id < MaxInFlight % SplitCount ? 1 : 0);
}

size_t TCsvFileReader::GetSplitCount() const {
    return SplitCount;
}


TFileChunk::TFileChunk(TFile file, THolder<IInputStream>&& stream, ui64 size)
    : File(file)
    , Stream(std::move(stream))
    , CountStream(MakeHolder<TCountingInput>(Stream.Get()))
    , Size(size) {
}

bool TFileChunk::ConsumeLine(TString& line) {
    ui64 prevCount = CountStream->Counter();
    line = NCsvFormat::TLinesSplitter(*CountStream).ConsumeLine();
    if (prevCount == CountStream->Counter() || prevCount >= Size) {
        return false;
    }
    return true;
}

ui64 TFileChunk::GetReadCount() const {
    return CountStream->Counter();
}

TCsvBatch::TCsvBatch(std::vector<TString>&& csvLines)
    : CsvLines(std::move(csvLines)) {
}

const std::vector<TString>& TCsvBatch::GetCsvLines() const {
    return CsvLines;
}

TImportFileClient::TImportFileClient(const TDriver& driver, const TClientCommand::TConfig& rootConfig)
    : TableClient(std::make_shared<NTable::TTableClient>(driver))
    , SchemeClient(std::make_shared<NScheme::TSchemeClient>(driver))
{
    RetrySettings
        .MaxRetries(TImportFileSettings::MaxRetries)
        .Idempotent(true)
        .Verbose(rootConfig.IsVerbose());
}

TStatus TImportFileClient::Import(const TVector<TString>& filePaths, const TString& dbPath, const TImportFileSettings& settings) {
    CurrentFileCount = filePaths.size();
    if (settings.Format_ == EDataFormat::Tsv && settings.Delimiter_ != "\t") {
        return MakeStatus(EStatus::BAD_REQUEST,
            TStringBuilder() << "Illegal delimiter for TSV format, only tab is allowed");
    }

    auto describeStatus = TableClient->RetryOperationSync(
        [this, dbPath](NTable::TSession session) {
            auto result = session.DescribeTable(dbPath).ExtractValueSync();
            if (result.IsSuccess()) {
                DbTableInfo = std::make_unique<const NTable::TTableDescription>(result.GetTableDescription());
            }
            return result;
        }, NTable::TRetryOperationSettings{RetrySettings}.MaxRetries(10));

    if (!describeStatus.IsSuccess()) {
        // TODO: Remove this after server fix: https://github.com/ydb-platform/ydb/issues/7791
        if (describeStatus.GetStatus() == EStatus::SCHEME_ERROR) {
            auto describePathResult = NDump::DescribePath(*SchemeClient, dbPath);
            if (describePathResult.GetStatus() != EStatus::SUCCESS) {
                return MakeStatus(EStatus::SCHEME_ERROR,
                    TStringBuilder() << describePathResult.GetIssues().ToString() << dbPath);
            }
        }
        return describeStatus;
    }

    UpsertSettings
        .OperationTimeout(settings.OperationTimeout_)
        .ClientTimeout(settings.ClientTimeout_);

    bool isStdoutInteractive = IsStdoutInteractive();
    size_t filePathsSize = filePaths.size();
    std::mutex progressWriteLock;
    std::atomic<ui64> globalProgress{0};

    TProgressBar progressBar(100);

    auto writeProgress = [&]() {
        ui64 globalProgressValue = globalProgress.load();
        std::lock_guard<std::mutex> lock(progressWriteLock);
        progressBar.SetProcess(globalProgressValue / filePathsSize);
    };

    StartTime = TInstant::Now();

    auto pool = CreateThreadPool(filePathsSize);
    ProcessingPool = CreateThreadPool(settings.Threads_, 0,
        IThreadPool::TParams().SetBlocking(true));
    TVector<NThreading::TFuture<TStatus>> asyncResults;

    size_t total_max_inflight = settings.Threads_ + settings.MaxInFlightRequests_;
    JobsInflight = MakeHolder<std::counting_semaphore<>>(total_max_inflight);
    RequestsInflight = MakeHolder<std::counting_semaphore<>>(settings.MaxInFlightRequests_);

    // If the single empty filename passed, read from stdin, else from the file

    for (const auto& filePath : filePaths) {
        auto func = [&, this] {
            std::unique_ptr<TFileInput> fileInput;
            std::optional<ui64> fileSizeHint;

            if (!filePath.empty()) {
                const TFsPath dataFile(filePath);

                if (!dataFile.Exists()) {
                    return MakeStatus(EStatus::BAD_REQUEST,
                        TStringBuilder() << "File does not exist: " << filePath);
                }

                if (!dataFile.IsFile()) {
                    return MakeStatus(EStatus::BAD_REQUEST,
                        TStringBuilder() << "Not a file: " << filePath);
                }

                TFile file(filePath, OpenExisting | RdOnly | Seq);
                i64 fileLength = file.GetLength();
                if (fileLength && fileLength >= 0) {
                    fileSizeHint = fileLength;
                }

                fileInput = std::make_unique<TFileInput>(file, settings.FileBufferSize_);
            }

            ProgressCallbackFunc progressCallback;

            if (isStdoutInteractive) {
                ui64 oldProgress = 0;
                progressCallback = [&, oldProgress](ui64 current, ui64 total) mutable {
                    ui64 progress = static_cast<ui64>((static_cast<double>(current) / total) * 100.0);
                    ui64 progressDiff = progress - oldProgress;
                    globalProgress.fetch_add(progressDiff);
                    oldProgress = progress;
                    writeProgress();
                };
            }

            IInputStream& input = fileInput ? *fileInput : Cin;

            switch (settings.Format_) {
                case EDataFormat::Default:
                case EDataFormat::Csv:
                case EDataFormat::Tsv:
                    if (settings.NewlineDelimited_) {
                        //return UpsertCsvByBlocks(filePath, dbPath, settings);
                        return UpsertCsvTest(filePath, dbPath, settings);
                    } else {
                        //return UpsertCsvSimple(input, filePath, settings.MaxInFlightRequests_, settings.BytesPerRequest_);
                        return UpsertCsv(input, dbPath, settings, filePath, fileSizeHint, progressCallback);
                    }
                case EDataFormat::Json:
                case EDataFormat::JsonUnicode:
                case EDataFormat::JsonBase64:
                    return UpsertJson(input, dbPath, settings, fileSizeHint, progressCallback);
                case EDataFormat::Parquet:
                    return UpsertParquet(filePath, dbPath, settings, progressCallback);
                default: ;
            }

            return MakeStatus(EStatus::BAD_REQUEST,
                        TStringBuilder() << "Unsupported format #" << (int) settings.Format_);
        };

        asyncResults.push_back(NThreading::Async(std::move(func), *pool));
    }

    NThreading::WaitAll(asyncResults).GetValueSync();
    for (const auto& asyncResult : asyncResults) {
        auto result = asyncResult.GetValueSync();
        if (!result.IsSuccess()) {
            return result;
        }
    }

    auto duration = TInstant::Now() - StartTime;
    progressBar.SetProcess(100);
    Cerr << "Elapsed: " << duration.SecondsFloat() << " sec. Total bytes read: " << (ui64)TotalBytesRead << ". Processing speed: "
        << (double)TotalBytesRead / duration.SecondsFloat() / 1024 / 1024  << " MB/s" << Endl;
    Cerr << "Total batches: " << TotalBatches << ". Total batches size: " << TotalBatchBytes << " bytes."
        //<< " Average batch size: " << TotalBatchBytes / TotalBatches << ". Total TValue building time: " << TotalTimes
        //<< ", Average TValue building time: " << TotalTimes / TotalBatches
        //<< ". Total CPU TValue building time: " << TotalCpuTimes
        //<< ", Average CPU TValue building time: " << TotalCpuTimes / TotalBatches
        << Endl;

    return MakeStatus(EStatus::SUCCESS);
}

inline
TAsyncStatus TImportFileClient::UpsertTValueBuffer(const TString& dbPath, TValueBuilder& builder) {
    auto upsert = [this, dbPath, rows = builder.Build()]
            (NYdb::NTable::TTableClient& tableClient) mutable -> TAsyncStatus {
        NYdb::TValue rowsCopy(rows.GetType(), rows.GetProto());
        return tableClient.BulkUpsert(dbPath, std::move(rowsCopy), UpsertSettings)
            .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                return NThreading::MakeFuture(status);
            });
    };
    return TableClient->RetryOperation(upsert, RetrySettings);
}

inline
TAsyncStatus TImportFileClient::UpsertTValueBuffer(const TString& dbPath, TValue&& rows) {
    /*return TableClient->BulkUpsert(dbPath, std::move(rows), UpsertSettings)
            .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                return NThreading::MakeFuture(status);
            });
    return NThreading::MakeFuture(MakeStatus());*/

    int counter = 0;
    static std::mutex requestMutex;
    static int requestCounter = 0;
    static std::random_device rd; // obtain a random number from hardware
    static std::mt19937 gen(rd()); // seed the generator
    static std::uniform_int_distribution<> distr(0, 1000); // define the range

    auto upsert = [this, dbPath, rows = std::move(rows), &counter]
            (NYdb::NTable::TTableClient& tableClient) mutable -> TAsyncStatus {
        ++counter;
        if (counter > 1) {
            Cerr << '#';
        }
        {
            std::lock_guard<std::mutex> lock(requestMutex);
            ++requestCounter;
            if (requestCounter % 1000 == 0) {
                Cerr << requestCounter << Endl;
            }
        }

        NYdb::TValue rowsCopy(rows.GetType(), rows.GetProto());
        return tableClient.BulkUpsert(dbPath, std::move(rowsCopy), UpsertSettings)
            .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                if (distr(gen) == 1) {
                    return NThreading::MakeFuture(MakeStatus(EStatus::UNAVAILABLE));
                }
                return NThreading::MakeFuture(status);
            });
    };
    return TableClient->RetryOperation(std::move(upsert), RetrySettings);
}

double diff_timespec(const struct timespec *time0, const struct timespec *time1) {
  return (time1->tv_sec - time0->tv_sec)
      + (time1->tv_nsec - time0->tv_nsec) / 1000000000.0;
}

namespace {
    void ParseCsvBuffer(TString&& data, std::map<TString, TStringBuf>& fields, const TVector<TString>& header, char delimeter) {
        NCsvFormat::CsvSplitter splitter(data, delimeter);
        auto headerIt = header.cbegin();
        do {
            TStringBuf token = splitter.Consume();
            fields[*headerIt] = token;
            ++headerIt;
        } while (splitter.Step());
    }

    void ParseCsvBufferLinear(const TString& data, std::vector<TStringBuf>& fields, char delimeter) {
        NCsvFormat::CsvSplitter splitter(data, delimeter);
        do {
            fields.emplace_back(splitter.Consume());
        } while (splitter.Step());
    }

    void BuildTValues(std::map<TString, TStringBuf>& fields, TValueBuilder& builder, const TType& type,
            const std::optional<TString>& nullValue) {
        builder.BeginStruct();
        TTypeParser parser(type);
        parser.OpenStruct();
        while (parser.TryNextMember()) {
            TString name = parser.GetMemberName();
            if (name == "__ydb_skip_column_name") {
                continue;
            }
            auto fieldIt = fields.find(name);
            builder.AddMember(name, FieldToValueSimple(parser, fieldIt->second, nullValue));
        }

        parser.CloseStruct();
        builder.EndStruct();
    }
}

TStatus TImportFileClient::UpsertCsvSimple(IInputStream& input,
                                     const TString& filePath,
                                     size_t maxInFlightRequests,
                                     ui64 bytesPerRequest) {
    TMaxInflightGetter inFlightGetter(maxInFlightRequests, CurrentFileCount);
    TCountingInput countInput(&input);
    NCsvFormat::TLinesSplitter splitter(countInput);
    TInstant fileStartTime = TInstant::Now();

    auto columnTypes = GetColumnTypes();
    ValidateTValueUpsertTable();

    TVector<TString> columns;
    for (const auto& column : DbTableInfo.get()->GetColumns()) {
        columns.push_back(column.Name);
    }
    std::optional<TString> nullValue = std::nullopt;
    TCsvParser parser = TCsvParser(std::move(columns), ',', nullValue, &columnTypes);

    TType lineType = parser.GetColumnsType();

    ui64 row = 1;
    ui64 batchRows = 0;
    ui64 nextBorder = VerboseModeReadSizeStep;
    ui64 batchBytes = 0;
    ui64 readBytes = 0;

    const TVector<TString>& header = parser.Header;
    char delimeter = parser.Delimeter;
    size_t columnCount = header.size();

    TString line;
    std::vector<TAsyncStatus> inFlightRequests;
    std::vector<TString> buffer;

    Y_UNUSED(ParseCsvBuffer);
    Y_UNUSED(BuildTValues);

    auto upsertCsvFromProto = [&, this]
            (ui64 row, std::vector<TString>&& buffer, ui64 batchSize) {
        std::vector<THolder<TTypeParser>> columnTypeParsers;
        columnTypeParsers.reserve(columnCount);
        for (const TString& columnName : header) {
            columnTypeParsers.push_back(MakeHolder<TTypeParser>(columnTypes.at(columnName)));
        }
        TInstant startBatchProcessing = TInstant::Now();
        struct timespec startCpuTime;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &startCpuTime);

        Ydb::Value listValue;
        auto* listItems = listValue.mutable_items();
        listItems->Reserve(buffer.size());

        for (auto&& line : buffer) {
            /*TCsvParser::TParseMetadata metadata = {row, filePath};*/
            Y_UNUSED(line);
            std::vector<TStringBuf> fields;
            ParseCsvBufferLinear(std::move(line), fields, delimeter);

            //AddItemToListProto(fields, listItems, lineType, nullValue);
            auto* structValue = listItems->Add();
            auto* structItems = structValue->mutable_items();
            structItems->Reserve(columnCount);
            for (auto [typeParserIt, fieldIt] = std::tuple{columnTypeParsers.begin(), fields.begin()}; typeParserIt != columnTypeParsers.end(); ++typeParserIt, ++fieldIt) {
                // TODO:
                //TString name = typeParser.GetMemberName();
                //if (name == "__ydb_skip_column_name") {
                //    continue;
                //}
                TTypeParser& typeParser = *typeParserIt->Get();
                typeParser.Reset();
                *structItems->Add() = FieldToValueSimple(typeParser, *fieldIt, nullValue).GetProto();
            }// while (typeParserIt != columnTypeParsers.end()/* && ++fieldIt != fields.end()*/);

            // TODO:
            // typeParser.Reset();

            //typeParser.CloseStruct();

            /*GetValueStatic(std::move(line), builder, lineType, TCsvParser::TParseMetadata{row, filePath},
                header, headerRow, delimeter, nullValue);*/
            ++row;
        }
        std::lock_guard<std::mutex> lock(StatsLock);
        struct timespec stopCpuTime;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stopCpuTime);
        TotalCpuTimes += diff_timespec(&startCpuTime, &stopCpuTime);
        TotalTimes += TInstant::Now() - startBatchProcessing;
        ++TotalBatches;
        TotalBatchBytes += batchSize;
        //Cerr << "List value: " << listValue.DebugString() << Endl;
        return MakeStatus();
        //return UpsertTValueBuffer(dbPath, builder).ExtractValueSync();
    };

    while (TString line = splitter.ConsumeLine()) {
        ++batchRows;
        if (line.empty()) {
            continue;
        }
        readBytes += line.size();
        batchBytes += line.size();

        buffer.push_back(line);

        if (readBytes >= nextBorder) {
            nextBorder += VerboseModeReadSizeStep;
            Cerr << "Processed " << 1.0 * readBytes / (1 << 20) << "Mb and " << row + batchRows << " records" << Endl;
        }

        if (batchBytes < bytesPerRequest) {
            continue;
        }

        auto asyncUpsertCSV = [&upsertCsvFromProto, row, buffer = std::move(buffer), batchBytes]() mutable {
            return upsertCsvFromProto(row, std::move(buffer), batchBytes);
        };
        row += batchRows;
        batchRows = 0;
        batchBytes = 0;
        //size_t capacity = buffer.capacity();
        buffer.clear();
        //buffer.reserve(capacity);

        inFlightRequests.push_back(NThreading::Async(asyncUpsertCSV, *ProcessingPool));

        auto status = WaitForQueue(inFlightGetter.GetCurrentMaxInflight(), inFlightRequests);
        if (!status.IsSuccess()) {
            return status;
        }
    }

    if (!buffer.empty() && countInput.Counter() > 0) {
        upsertCsvFromProto(row, std::move(buffer), batchBytes);
    }

    auto waitResult = WaitForQueue(0, inFlightRequests);

    TotalBytesRead += readBytes;

    TStringBuilder builder;
    TDuration fileProcessingTime = TInstant::Now() - fileStartTime;
    builder << Endl << "File " << filePath << " processed in " << fileProcessingTime << ", "
        << (double)readBytes / fileProcessingTime.SecondsFloat() / 1024 / 1024  << " MB/s" << Endl;
        Cerr << builder << Endl;
    return waitResult;
}

TStatus TImportFileClient::UpsertCsv(IInputStream& input,
                                     const TString& dbPath,
                                     const TImportFileSettings& settings,
                                     const TString& filePath,
                                     std::optional<ui64> inputSizeHint,
                                     ProgressCallbackFunc & progressCallback) {

    TMaxInflightGetter inFlightGetter(settings.MaxInFlightRequests_, CurrentFileCount);
    Y_UNUSED(dbPath);
    TCountingInput countInput(&input);
    NCsvFormat::TLinesSplitter splitter(countInput);
    TInstant fileStartTime = TInstant::Now();
    static std::atomic<int> workerCounter = 0;

    const auto columnTypes = GetColumnTypes();
    ValidateTValueUpsertTable();

    TCsvParser parser;
    bool removeLastDelimiter = false;
    InitCsvParser(parser, removeLastDelimiter, splitter, settings, &columnTypes, DbTableInfo.get());

    for (ui32 i = 0; i < settings.SkipRows_; ++i) {
        splitter.ConsumeLine();
    }

    TType lineType = parser.GetColumnsType();
    TType listType = TTypeBuilder().List(lineType).Build();

    ui64 row = settings.SkipRows_ + settings.Header_ + 1;
    ui64 batchRows = 0;
    ui64 nextBorder = VerboseModeReadSizeStep;
    ui64 batchBytes = 0;
    ui64 readBytes = 0;

    TVector<TString> header = parser.Header;
    size_t columnCount = header.size();
    TString headerRow = parser.HeaderRow;
    char delimeter = parser.Delimeter;
    Y_UNUSED(delimeter);
    std::optional<TString> nullValue = parser.NullValue;

    TString line;
    std::vector<TAsyncStatus> inFlightRequests;
    std::vector<TString> buffer;
    TInstant lastProgressTime = TInstant::Now();
    TDuration progressInterval = TDuration::Seconds(5);

    auto upsertCsvFunc = [&, this]
            (std::shared_ptr<TCsvBatch> batchHolder/*, ui64 batchSize*/) {
        /*auto str = TStringBuilder() << "[" << (ui64)TotalInflight << "]";
        Cerr << str;
        ++TotalInflight;*/
        auto buildTValueFromStrings = [batchHolder, columnCount, &header, &columnTypes, &delimeter, &listType, &nullValue]() {
            std::vector<THolder<TTypeParser>> columnTypeParsers;
            columnTypeParsers.reserve(columnCount);
            for (const TString& columnName : header) {
                columnTypeParsers.push_back(MakeHolder<TTypeParser>(columnTypes.at(columnName)));
            }
            /*TInstant startBatchProcessing = TInstant::Now();
            struct timespec startCpuTime;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &startCpuTime);*/

            Ydb::Value listValue;
            auto* listItems = listValue.mutable_items();
            auto& batch = batchHolder->GetCsvLines();
            listItems->Reserve(batch.size());
            for (const auto& line : batch) {
                std::vector<TStringBuf> fields;
                ParseCsvBufferLinear(line, fields, delimeter);
                auto* structValue = listItems->Add();
                auto* structItems = structValue->mutable_items();
                structItems->Reserve(columnCount);
                for (auto [typeParserIt, fieldIt] = std::tuple{columnTypeParsers.begin(), fields.begin()}; typeParserIt != columnTypeParsers.end(); ++typeParserIt, ++fieldIt) {
                    // TODO:
                    //TString name = typeParser.GetMemberName();
                    //if (name == "__ydb_skip_column_name") {
                    //    continue;
                    //}
                    TTypeParser& typeParser = *typeParserIt->Get();
                    typeParser.Reset();
                    *structItems->Add() = FieldToValueSimple(typeParser, *fieldIt, nullValue).GetProto();
                }
                //++row;
            }
            return TValue(listType, std::move(listValue));
        };
        //Y_UNUSED(listTypedValue);
        /*{
            std::lock_guard<std::mutex> lock(StatsLock);
            struct timespec stopCpuTime;
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stopCpuTime);
            TotalCpuTimes += diff_timespec(&startCpuTime, &stopCpuTime);
            TotalTimes += TInstant::Now() - startBatchProcessing;
            ++TotalBatches;
            TotalBatchBytes += batchSize;
        }*/
        //return MakeStatus();


        int retryCounter = 0;
        static std::mutex requestMutex;
        static int requestCounter = 0;
        static std::random_device rd; // obtain a random number from hardware
        static std::mt19937 gen(rd()); // seed the generator
        static std::uniform_int_distribution<> distr(0, 1000); // define the range

        auto retryFunc = [this, &dbPath, &retryCounter, buildTValueFromStrings = std::move(buildTValueFromStrings)]
                (NYdb::NTable::TTableClient& tableClient) mutable -> TAsyncStatus {
            auto buildTValueAndSendRequest = [this, &buildTValueFromStrings, &dbPath, &tableClient]() {
                TValue rows = buildTValueFromStrings();

                //NYdb::TValue rowsCopy(rows.GetType(), rows.GetProto());
                return tableClient.BulkUpsert(dbPath, std::move(rows), UpsertSettings)
                    .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                        NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                        if (distr(gen) == 1) {
                            return NThreading::MakeFuture(MakeStatus(EStatus::UNAVAILABLE));
                        }
                        return NThreading::MakeFuture(status);
                    });
            };
            ++retryCounter;
            if (retryCounter > 1) {
                Cerr << '#';
            }
            {
                std::lock_guard<std::mutex> lock(requestMutex);
                ++requestCounter;
                if (requestCounter % 1000 == 0) {
                    Cerr << requestCounter << Endl;
                }
            }
            // Running heavy building task on processing pool:
            return NThreading::Async(buildTValueAndSendRequest, *ProcessingPool);
        };

        if (!RequestsInflight->try_acquire()) {
            Cerr << '@';
            RequestsInflight->acquire();
        }
        TableClient->RetryOperation(std::move(retryFunc), RetrySettings)
                .Apply([this](const TAsyncStatus& asyncStatus) {
                    NYdb::TStatus status = asyncStatus.GetValueSync();
                    if (!status.IsSuccess()) {
                        std::lock_guard<std::mutex> lock(StatusLock);
                        bool expected = false;
                        if (Failed.compare_exchange_strong(expected, true)) {
                            ErrorStatus = MakeHolder<TStatus>(status);
                        }
                    }
                    --workerCounter;

                    RequestsInflight->release();
                    JobsInflight->release();
                    return NThreading::MakeFuture(status);
                });
        //--TotalInflight;
    };

    //TDuration waitTime = TDuration::Seconds(0);
    while (TString line = splitter.ConsumeLine()) {
        ++batchRows;
        if (line.empty()) {
            continue;
        }
        readBytes += line.size();
        batchBytes += line.size();

        if (removeLastDelimiter) {
            if (!line.EndsWith(settings.Delimiter_)) {
                return MakeStatus(EStatus::BAD_REQUEST,
                        "According to the header, lines should end with a delimiter");
            }
            line.erase(line.size() - settings.Delimiter_.size());
        }

        buffer.push_back(line);

        if (readBytes >= nextBorder && settings.Verbose_) {
            nextBorder += VerboseModeReadSizeStep;
            Cerr << "Processed " << 1.0 * readBytes / (1 << 20) << "Mb and " << row + batchRows << " records" << ", "
                << (double)readBytes / (TInstant::Now() - fileStartTime).SecondsFloat() / 1024 / 1024 << "MiB/s" << Endl;
        }

        if (batchBytes < settings.BytesPerRequest_) {
            continue;
        }

        Y_UNUSED(inputSizeHint);
        Y_UNUSED(progressCallback);
        Y_UNUSED(lastProgressTime);
        Y_UNUSED(progressInterval);

        /*if (inputSizeHint && progressCallback && TInstant::Now() - lastProgressTime > progressInterval) {
            progressCallback(readBytes, *inputSizeHint);
            lastProgressTime = TInstant::Now();
        }*/

        /*auto asyncUpsertCSV = [&upsertCsv, row, buffer = std::move(buffer), batchBytes]() mutable {
            return upsertCsv(row, std::move(buffer), batchBytes);
        };*/

        size_t capacity = buffer.capacity();
        auto workerFunc = [&upsertCsvFunc, batch = std::make_shared<TCsvBatch>(std::move(buffer))/*, batchBytes*/]() mutable {

            ++workerCounter;
            int currentValue = workerCounter;
            TStringBuilder str = TStringBuilder() << '<' << currentValue << '>';
            Cerr << str;
            upsertCsvFunc(batch/*, batchBytes*/);
        };
        row += batchRows;
        batchRows = 0;
        batchBytes = 0;
        buffer.clear();
        buffer.reserve(capacity);

        JobsInflight->acquire();

        //TInstant waitStartTime = TInstant::Now();
        if (!ProcessingPool->AddFunc(workerFunc)) {
            return MakeStatus(EStatus::INTERNAL_ERROR, "Couldn't add worker func");
        }
        //inFlightRequests.push_back(NThreading::Async(asyncUpsertCSV, *pool));

        //auto status = WaitForQueue(inFlightGetter.GetCurrentMaxInflight(), inFlightRequests);
        //waitTime += TInstant::Now() - waitStartTime;

        if (Failed) {
            std::lock_guard<std::mutex> lock(StatusLock);
            return *ErrorStatus;
        }
    }

    if (!buffer.empty() && countInput.Counter() > 0) {
        upsertCsvFunc(std::make_shared<TCsvBatch>(std::move(buffer))/*, batchBytes*/);
    }

    size_t total_max_inflight = settings.Threads_ + settings.MaxInFlightRequests_;

    for (size_t i = 0; i < total_max_inflight; ++i) {
        JobsInflight->acquire();
    }
    TotalBytesRead += readBytes;

    //auto waitResult = WaitForQueue(0, inFlightRequests);

    if (settings.Verbose_) {
        TStringBuilder builder;
        TDuration fileProcessingTime = TInstant::Now() - fileStartTime;
        builder << Endl << "File " << filePath << " processed in " << fileProcessingTime << ", "
            << (double)readBytes / fileProcessingTime.SecondsFloat() / 1024 / 1024  << " MB/s" << Endl
            //<< ", wait time: " << waitTime
             << Endl;
            Cerr << builder;
    }
    if (Failed) {
        std::lock_guard<std::mutex> lock(StatusLock);
        return *ErrorStatus;
    } else {
        return MakeStatus();
    }
}

TStatus TImportFileClient::UpsertCsvBlock(TFileChunk& chunk,
                                     const TString& dbPath,
                                     const TImportFileSettings& settings,
                                     const TString& filePath,
                                     const TType& lineType,
                                     const TVector<TString>& header,
                                     const TString& headerRow,
                                     char delimeter,
                                     const std::optional<TString>& nullValue,
                                     bool removeLastDelimiter) {

    TMaxInflightGetter inFlightGetter(settings.MaxInFlightRequests_, CurrentFileCount);
    Y_UNUSED(dbPath);
    Y_UNUSED(lineType);
    Y_UNUSED(header);
    Y_UNUSED(headerRow);
    Y_UNUSED(delimeter);
    Y_UNUSED(nullValue);
    TInstant chunkStartTime = TInstant::Now();

    ui64 row = settings.Header_ + 1; // -header for all but 1st
    ui64 batchRows = 0;
    ui64 nextBorder = VerboseModeReadSizeStep;
    ui64 batchBytes = 0;
    ui64 readBytes = 0;

    TString line;
    std::vector<TAsyncStatus> inFlightRequests;
    std::vector<TString> buffer;

    auto upsertCsv = [&, this](ui64 row, std::vector<TString>&& buffer, ui64 batchSize) {
        auto str = TStringBuilder() << "[" << (ui64)TotalInflight << "]";
        Cerr << str;
        ++TotalInflight;
        TInstant startBatchProcessing = TInstant::Now();
        struct timespec startCpuTime;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &startCpuTime);

        TValueBuilder builder;
        builder.BeginList();
        for (auto&& line : buffer) {
            /*TCsvParser::TParseMetadata metadata = {row, filePath};*/
            Y_UNUSED(line);
            //Y_UNUSED(metadata);
            /*builder.AddListItem();
            GetValueStatic(std::move(line), builder, lineType, TCsvParser::TParseMetadata{row, filePath},
                header, headerRow, delimeter, nullValue);*/
            ++row;
        }
        builder.EndList();
        --TotalInflight;
        std::lock_guard<std::mutex> lock(StatsLock);
        struct timespec stopCpuTime;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &stopCpuTime);
        TotalCpuTimes += diff_timespec(&startCpuTime, &stopCpuTime);
        TotalTimes += TInstant::Now() - startBatchProcessing;
        ++TotalBatches;
        TotalBatchBytes += batchSize;
        return MakeStatus();
        //return UpsertTValueBuffer(dbPath, builder).ExtractValueSync();
    };

    while (chunk.ConsumeLine(line)) {
        ++batchRows;
        if (line.empty()) {
            continue;
        }
        readBytes += line.size();
        batchBytes += line.size();

        if (removeLastDelimiter) {
            if (!line.EndsWith(settings.Delimiter_)) {
                return MakeStatus(EStatus::BAD_REQUEST,
                        "According to the header, lines should end with a delimiter");
            }
            line.erase(line.size() - settings.Delimiter_.size());
        }

        buffer.push_back(line);

        if (readBytes >= nextBorder && settings.Verbose_) {
            nextBorder += VerboseModeReadSizeStep;
            Cerr << "Processed " << 1.0 * readBytes / (1 << 20) << "Mb and " << row + batchRows << " records" << Endl;
        }

        if (batchBytes < settings.BytesPerRequest_) {
            continue;
        }

        auto asyncUpsertCSV = [&upsertCsv, row, buffer = std::move(buffer), batchBytes]() mutable {
            return upsertCsv(row, std::move(buffer), batchBytes);
        };
        row += batchRows;
        batchRows = 0;
        batchBytes = 0;
        buffer.clear();

        inFlightRequests.push_back(NThreading::Async(asyncUpsertCSV, *ProcessingPool));

        auto status = WaitForQueue(inFlightGetter.GetCurrentMaxInflight(), inFlightRequests);
        if (!status.IsSuccess()) {
            return status;
        }
    }

    if (!buffer.empty() && chunk.GetReadCount() > 0) {
        upsertCsv(row, std::move(buffer), batchBytes);
    }

    auto waitResult = WaitForQueue(0, inFlightRequests);

    TotalBytesRead += readBytes;

    if (settings.Verbose_) {
        TStringBuilder builder;
        TDuration fileProcessingTime = TInstant::Now() - chunkStartTime;
        builder << Endl << "File chunk for " << filePath << " processed in " << fileProcessingTime << ", "
            << (double)readBytes / fileProcessingTime.SecondsFloat() / 1024 / 1024  << " MB/s" << Endl;
            Cerr << builder << Endl;
    }
    return waitResult;
}

TStatus TImportFileClient::UpsertCsvTest(const TString& filePath,
                                            const TString& dbPath,
                                            const TImportFileSettings& settings) {
    TMaxInflightGetter inFlightGetter(settings.MaxInFlightRequests_, CurrentFileCount);
    TString headerRow;
    TCsvFileReader splitter(filePath, settings, headerRow, inFlightGetter);
    if (settings.Verbose_) {
        Cerr << "Split file " << filePath << " into " << splitter.GetSplitCount() << " chunks." << Endl;
    }
    Y_UNUSED(dbPath);
    TInstant fileStartTime = TInstant::Now();
    std::atomic<ui64> fileBytesRead = 0;

    auto columnTypes = GetColumnTypes();
    ValidateTValueUpsertTable();

    TCsvParser parser;
    bool removeLastDelimiter = false;
    TStringInput headerInput(headerRow);
    NCsvFormat::TLinesSplitter headerSplitter(headerInput, settings.Delimiter_[0]);
    InitCsvParser(parser, removeLastDelimiter, headerSplitter, settings, &columnTypes, DbTableInfo.get());

    TType lineType = parser.GetColumnsType();
    TVector<TString> header = parser.Header;
    char delimeter = parser.Delimeter;
    std::optional<TString> nullValue = parser.NullValue;

    TVector<TAsyncStatus> threadResults(splitter.GetSplitCount());

    for (size_t threadId = 0; threadId < splitter.GetSplitCount(); ++threadId) {
        auto loadCsv = [&, threadId] () {
            auto& chunk = splitter.GetChunk(threadId);
            return UpsertCsvBlock(chunk, dbPath, settings, filePath, lineType,
                header, headerRow, delimeter, nullValue, removeLastDelimiter);
        };
        threadResults[threadId] = NThreading::Async(loadCsv, *ProcessingPool);
    }
    NThreading::WaitAll(threadResults).Wait();
    for (size_t i = 0; i < splitter.GetSplitCount(); ++i) {
        if (!threadResults[i].GetValueSync().IsSuccess()) {
            return threadResults[i].GetValueSync();
        }
    }

    if (settings.Verbose_) {
        TStringBuilder builder;
        TDuration fileProcessingTime = TInstant::Now() - fileStartTime;
        builder << "File " << filePath << " processed in " << fileProcessingTime << ", "
            << (double)fileBytesRead / fileProcessingTime.SecondsFloat() / 1024 / 1024  << " MB/s" << Endl;
            Cerr << builder << Endl;
    }
    return MakeStatus();
}

TStatus TImportFileClient::UpsertCsvByBlocks(const TString& filePath,
                                             const TString& dbPath,
                                             const TImportFileSettings& settings) {

    TMaxInflightGetter inFlightGetter(settings.MaxInFlightRequests_, CurrentFileCount);
    TString headerRow;
    TCsvFileReader splitter(filePath, settings, headerRow, inFlightGetter);
    if (settings.Verbose_) {
        Cerr << "Split file " << filePath << " into " << splitter.GetSplitCount() << " chunks." << Endl;
    }
    Y_UNUSED(dbPath);
    TInstant fileStartTime = TInstant::Now();
    std::atomic<ui64> fileBytesRead = 0;

    auto columnTypes = GetColumnTypes();
    ValidateTValueUpsertTable();

    TCsvParser parser;
    bool removeLastDelimiter = false;
    TStringInput headerInput(headerRow);
    NCsvFormat::TLinesSplitter headerSplitter(headerInput, settings.Delimiter_[0]);
    InitCsvParser(parser, removeLastDelimiter, headerSplitter, settings, &columnTypes, DbTableInfo.get());

    TType lineType = parser.GetColumnsType();

    TVector<TAsyncStatus> threadResults(splitter.GetSplitCount());
    THolder<IThreadPool> pool = CreateThreadPool(splitter.GetSplitCount());
    for (size_t threadId = 0; threadId < splitter.GetSplitCount(); ++threadId) {
        auto loadCsv = [&, threadId] () {
            auto upsertCsv = [&](std::vector<TString>&& buffer) {
                TValueBuilder builder;
                builder.BeginList();
                for (auto&& line : buffer) {
                    builder.AddListItem();
                    parser.GetValue(std::move(line), builder, lineType, TCsvParser::TParseMetadata{std::nullopt, filePath});
                }
                builder.EndList();
                auto promise = NThreading::NewPromise<TStatus>();
                promise.SetValue(MakeStatus());
                return promise.GetFuture();
                //return UpsertTValueBuffer(dbPath, builder);
            };
            std::vector<TAsyncStatus> inFlightRequests;
            std::vector<TString> buffer;
            ui32 idx = settings.SkipRows_;
            ui64 readBytes = 0;
            ui64 batchBytes = 0;
            ui64 nextBorder = VerboseModeReadSizeStep;
            TAsyncStatus status;
            TString line;
            TInstant chunkStartTime = TInstant::Now();
            while (splitter.GetChunk(threadId).ConsumeLine(line)) {
                if (line.empty()) {
                    continue;
                }
                readBytes += line.size();
                batchBytes += line.size();
                if (removeLastDelimiter) {
                    if (!line.EndsWith(settings.Delimiter_)) {
                        return MakeStatus(EStatus::BAD_REQUEST,
                                "According to the header, lines should end with a delimiter");
                    }
                    line.erase(line.size() - settings.Delimiter_.size());
                }
                buffer.push_back(line);
                /*if (line.Contains(';')) {
                    return MakeStatus(EStatus::BAD_REQUEST,
                            "TEST ERROR: Found semicolon in csv");
                }*/
                ++idx;
                if (readBytes >= nextBorder && settings.Verbose_) {
                    nextBorder += VerboseModeReadSizeStep;
                    TStringBuilder builder;
                    builder << "Processed " << 1.0 * readBytes / (1 << 20) << "Mb and " << idx << " records" << Endl;
                    Cerr << builder;
                    Cerr << "!";
                }
                if (batchBytes >= settings.BytesPerRequest_) {
                    batchBytes = 0;
                    int threadLimit = splitter.GetThreadLimit(threadId);

                    auto status = WaitForQueue(threadLimit, inFlightRequests);
                    if (!status.IsSuccess()) {
                        return status;
                    }

                    inFlightRequests.push_back(upsertCsv(std::move(buffer)));
                    buffer.clear();
                }
            }

            if (!buffer.empty() && splitter.GetChunk(threadId).GetReadCount() != 0) {
                inFlightRequests.push_back(upsertCsv(std::move(buffer)));
            }
            TotalBytesRead += readBytes;
            fileBytesRead += readBytes;

            if (settings.Verbose_) {
                TStringBuilder builder;
                TDuration chunkProcessingTime = TInstant::Now() - chunkStartTime;
                builder << "Chunk " << filePath << "#" << threadId << " processed " << readBytes << " bytes in "
                    << chunkProcessingTime << ": " << (double)readBytes / chunkProcessingTime.SecondsFloat() / 1024 / 1024
                     << " MB/s" << Endl;
                    Cerr << builder << Endl;
            }

            return WaitForQueue(0, inFlightRequests);
        };
        threadResults[threadId] = NThreading::Async(loadCsv, *pool);
    }
    NThreading::WaitAll(threadResults).Wait();
    for (size_t i = 0; i < splitter.GetSplitCount(); ++i) {
        if (!threadResults[i].GetValueSync().IsSuccess()) {
            return threadResults[i].GetValueSync();
        }
    }

    if (settings.Verbose_) {
        TStringBuilder builder;
        TDuration fileProcessingTime = TInstant::Now() - fileStartTime;
        builder << "File " << filePath << " processed in " << fileProcessingTime << ", "
            << (double)fileBytesRead / fileProcessingTime.SecondsFloat() / 1024 / 1024  << " MB/s" << Endl;
            Cerr << builder << Endl;
    }
    return MakeStatus();
}

TStatus TImportFileClient::UpsertJson(IInputStream& input, const TString& dbPath, const TImportFileSettings& settings,
                                    std::optional<ui64> inputSizeHint, ProgressCallbackFunc & progressCallback) {
    const TType tableType = GetTableType();
    ValidateTValueUpsertTable();

    TMaxInflightGetter inFlightGetter(settings.MaxInFlightRequests_, CurrentFileCount);
    THolder<IThreadPool> pool = CreateThreadPool(settings.Threads_);

    ui64 readBytes = 0;
    ui64 batchBytes = 0;

    TString line;
    std::vector<TString> batchLines;
    std::vector<TAsyncStatus> inFlightRequests;

    auto upsertJson = [&](const std::vector<TString>& batchLines) {
        TValueBuilder batch;
        batch.BeginList();

        for (auto &line : batchLines) {
            batch.AddListItem(JsonToYdbValue(line, tableType, settings.BinaryStringsEncoding_));
        }

        batch.EndList();

        auto value = UpsertTValueBuffer(dbPath, batch);
        return value.ExtractValueSync();
    };

    while (size_t size = input.ReadLine(line)) {
        batchLines.push_back(line);
        batchBytes += size;
        readBytes += size;

        if (inputSizeHint && progressCallback) {
            progressCallback(readBytes, *inputSizeHint);
        }

        if (batchBytes < settings.BytesPerRequest_) {
            continue;
        }

        batchBytes = 0;

        auto asyncUpsertJson = [&, batchLines = std::move(batchLines)]() {
            return upsertJson(batchLines);
        };

        batchLines.clear();

        inFlightRequests.push_back(NThreading::Async(asyncUpsertJson, *pool));

        auto status = WaitForQueue(inFlightGetter.GetCurrentMaxInflight(), inFlightRequests);
        if (!status.IsSuccess()) {
            return status;
        }
    }

    if (!batchLines.empty()) {
        upsertJson(std::move(batchLines));
    }

    return WaitForQueue(0, inFlightRequests);
}

TStatus TImportFileClient::UpsertParquet([[maybe_unused]] const TString& filename,
                                         [[maybe_unused]] const TString& dbPath,
                                         [[maybe_unused]] const TImportFileSettings& settings,
                                         [[maybe_unused]] ProgressCallbackFunc & progressCallback) {
#if defined(_WIN64) || defined(_WIN32) || defined(__WIN32__)
    return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Not supported on Windows");
#else
    std::shared_ptr<arrow::io::ReadableFile> infile;
    arrow::Result<std::shared_ptr<arrow::io::ReadableFile>> fileResult = arrow::io::ReadableFile::Open(filename);
    if (!fileResult.ok()) {
        return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Unable to open parquet file:" << fileResult.status().ToString());
    }
    std::shared_ptr<arrow::io::ReadableFile> readableFile = *fileResult;

    std::unique_ptr<parquet::arrow::FileReader> fileReader;

    arrow::Status st;
    st = parquet::arrow::OpenFile(readableFile, arrow::default_memory_pool(), &fileReader);
    if (!st.ok()) {
        return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Error while initializing arrow FileReader: " << st.ToString());
    }

    auto metadata = parquet::ReadMetaData(readableFile);
    const i64 numRows = metadata->num_rows();
    const i64 numRowGroups = metadata->num_row_groups();

    std::vector<int> row_group_indices(numRowGroups);
    for (i64 i = 0; i < numRowGroups; i++) {
        row_group_indices[i] = i;
    }

    std::unique_ptr<arrow::RecordBatchReader> reader;

    st = fileReader->GetRecordBatchReader(row_group_indices, &reader);
    if (!st.ok()) {
        return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Error while getting RecordBatchReader: " << st.ToString());
    }

    THolder<IThreadPool> pool = CreateThreadPool(settings.Threads_);

    std::atomic<ui64> uploadedRows = 0;
    auto uploadedRowsCallback = [&](ui64 rows) {
        ui64 uploadedRowsValue = uploadedRows.fetch_add(rows);

        if (progressCallback) {
            progressCallback(uploadedRowsValue + rows, numRows);
        }
    };

    std::vector<TAsyncStatus> inFlightRequests;

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;

        st = reader->ReadNext(&batch);
        if (!st.ok()) {
            return MakeStatus(EStatus::BAD_REQUEST, TStringBuilder() << "Error while reading next RecordBatch" << st.ToString());
        }

        // The read function will return null at the end of data stream.
        if (!batch) {
            break;
        }

        auto upsertParquetBatch = [&, batch = std::move(batch)]() {
            const TString strSchema = NYdb_cli::NArrow::SerializeSchema(*batch->schema());
            const size_t totalSize = NYdb_cli::NArrow::GetBatchDataSize(batch);
            const size_t sliceCount =
                (totalSize / (size_t)settings.BytesPerRequest_) + (totalSize % settings.BytesPerRequest_ != 0 ? 1 : 0);
            const i64 rowsInSlice = batch->num_rows() / sliceCount;

            for (i64 currentRow = 0; currentRow < batch->num_rows(); currentRow += rowsInSlice) {
                std::stack<std::shared_ptr<arrow::RecordBatch>> rowsToSendBatches;

                if (currentRow + rowsInSlice < batch->num_rows()) {
                    rowsToSendBatches.push(batch->Slice(currentRow, rowsInSlice));
                } else {
                    rowsToSendBatches.push(batch->Slice(currentRow));
                }

                do {
                    auto rowsBatch = std::move(rowsToSendBatches.top());
                    rowsToSendBatches.pop();

                    // Nothing to send. Continue.
                    if (rowsBatch->num_rows() == 0) {
                        continue;
                    }

                    // Logarithmic approach to find number of rows fit into the byte limit.
                    if (rowsBatch->num_rows() == 1 || NYdb_cli::NArrow::GetBatchDataSize(rowsBatch) < settings.BytesPerRequest_) {
                        // Single row or fits into the byte limit.
                        auto value = UpsertParquetBuffer(dbPath, NYdb_cli::NArrow::SerializeBatchNoCompression(rowsBatch), strSchema);
                        auto status = value.ExtractValueSync();
                        if (!status.IsSuccess())
                            return status;

                        uploadedRowsCallback(rowsBatch->num_rows());
                    } else {
                        // Split current slice.
                        i64 halfLen = rowsBatch->num_rows() / 2;
                        rowsToSendBatches.push(rowsBatch->Slice(halfLen));
                        rowsToSendBatches.push(rowsBatch->Slice(0, halfLen));
                    }
                } while (!rowsToSendBatches.empty());
            };

            return MakeStatus();
        };

        inFlightRequests.push_back(NThreading::Async(upsertParquetBatch, *pool));

        auto status = WaitForQueue(settings.MaxInFlightRequests_, inFlightRequests);
        if (!status.IsSuccess()) {
            return status;
        }
    }

    return WaitForQueue(0, inFlightRequests);
#endif
}

inline
TAsyncStatus TImportFileClient::UpsertParquetBuffer(const TString& dbPath, const TString& buffer, const TString& strSchema) {
    auto upsert = [this, dbPath, buffer, strSchema](NYdb::NTable::TTableClient& tableClient) -> TAsyncStatus {
        return tableClient.BulkUpsert(dbPath, NTable::EDataFormat::ApacheArrow, buffer, strSchema, UpsertSettings)
            .Apply([](const NYdb::NTable::TAsyncBulkUpsertResult& bulkUpsertResult) {
                NYdb::TStatus status = bulkUpsertResult.GetValueSync();
                return NThreading::MakeFuture(status);
            });
        };
    return TableClient->RetryOperation(upsert, RetrySettings);
}

TType TImportFileClient::GetTableType() {
    TTypeBuilder typeBuilder;
    typeBuilder.BeginStruct();
    Y_ENSURE_BT(DbTableInfo);
    const auto& columns = DbTableInfo->GetTableColumns();
    for (auto it = columns.begin(); it != columns.end(); it++) {
        typeBuilder.AddMember((*it).Name, (*it).Type);
    }
    typeBuilder.EndStruct();
    return typeBuilder.Build();
}

std::map<TString, TType> TImportFileClient::GetColumnTypes() {
    std::map<TString, TType> columnTypes;
    Y_ENSURE_BT(DbTableInfo);
    const auto& columns = DbTableInfo->GetTableColumns();
    for (auto it = columns.begin(); it != columns.end(); it++) {
        columnTypes.insert({(*it).Name, (*it).Type});
    }
    return columnTypes;
}

void TImportFileClient::ValidateTValueUpsertTable() {
    auto columnTypes = GetColumnTypes();
    bool hasPgType = false;
    for (const auto& [_, type] : columnTypes) {
        if (TTypeParser(type).GetKind() == TTypeParser::ETypeKind::Pg) {
            hasPgType = true;
            break;
        }
    }
    Y_ENSURE_BT(DbTableInfo);
    if (DbTableInfo->GetStoreType() == NTable::EStoreType::Column && hasPgType) {
        throw TMisuseException() << "Import into column table with Pg type columns in not supported";
    }
}

}
}
