# Справка по метрикам

{% note info %}

См. также [{#T}](grafana-dashboards.md).

{% endnote %}

## Метрики использования ресурсов {#resources}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`resources.storage.used_bytes`<br/>`IGAUGE`, байты  | Размер пользовательских и служебных данных, сохраненных в распределенном сетевом хранилище. `resources.storage.used_bytes` = `resources.storage.table.used_bytes` + `resources.storage.topic.used_bytes`.
`resources.storage.table.used_bytes`<br/>`IGAUGE`, байты  | Размер пользовательских и служебных данных, сохраненных таблицами в распределенном сетевом хранилище. К служебным данным относятся данные первичного, [вторичных индексов](../../../concepts/glossary.md#secondary-index) и [векторных индексов](../../../concepts/glossary.md#vector-index).
`resources.storage.topic.used_bytes`<br/>`IGAUGE`, байты  | Размер распределенного сетевого хранилища, используемого топиками. Равен сумме значений `topic.storage_bytes` всех топиков.
`resources.storage.limit_bytes`<br/>`IGAUGE`, байты  | Ограничение на размер пользовательских и служебных данных, которые база данных может сохранить в распределенном сетевом хранилище.

## Метрики GRPC API общие {#grpc_api}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`api.grpc.request.bytes`<br/>`RATE`, байты | Размер запросов, которые получены базой данных в определенный период времени.<br/>Метки:<br/>- _api_service_ – название сервиса gRPC API, например `table` или `data_streams`.<br/>- _method_ – название метода сервиса gRPC API, например `ExecuteDataQuery` (для сервиса `table`), или `PutRecord`, `GetRecords` (для сервиса `data_streams`).
`api.grpc.request.dropped_count`<br/>`RATE`, штуки | Количество запросов, обработка которых была прекращена на транспортном (gRPC) уровне из-за ошибки.<br/>Метки:<br/>- _api_service_ – название сервиса gRPC API, например `table`.<br/>- _method_ – название метода сервиса gRPC API, например `ExecuteDataQuery`.
`api.grpc.request.inflight_count`<br/>`IGAUGE`, штуки | Количество запросов, которые одновременно обрабатываются базой данных в определенный период времени.<br/>Метки:<br/>- _api_service_ – название сервиса gRPC API, например `table`.<br/>- _method_ – название метода сервиса gRPC API, например `ExecuteDataQuery`.
`api.grpc.request.inflight_bytes`<br/>`IGAUGE`, байты | Размер запросов, которые одновременно обрабатываются базой данных в определенный период времени.<br/>Метки:<br/>- _api_service_ – название сервиса gRPC API, например `table`.<br/>- _method_ – название метода сервиса gRPC API, например `ExecuteDataQuery`.
`api.grpc.response.bytes`<br/>`RATE`, байты | Размер ответов, которые отправлены базой данный в определенный период времени.<br/>Метки:<br/>- _api_service_ – название сервиса gRPC API, например `table`.<br/>- _method_ – название метода сервиса gRPC API, например `ExecuteDataQuery`.
`api.grpc.response.count`<br/>`RATE`, штуки | Количество ответов, которые отправлены базой в определенный период времени.<br/>Метки:<br/>- _api_service_ – название сервиса gRPC API, например `table`.<br/>- _method_ – название метода сервиса gRPC API, например `ExecuteDataQuery`.<br/>- _status_ – статус выполнения запроса, подробнее статусы описаны в разделе [Обработка ошибок](../../../reference/ydb-sdk/error_handling.md).
`api.grpc.response.dropped_count`<br/>`RATE`, штуки | Количество ответов, отправка которых была прекращена на на транспортном (gRPC) уровне из-за ошибки.<br/>Метки:<br/>- _api_service_ – название сервиса gRPC API, например `table`.<br/>- _method_ – название метода сервиса gRPC API, например `ExecuteDataQuery`.
`api.grpc.response.issues`<br/>`RATE`, штуки | Количество ошибок определенного типа, возникших при выполнении запросов в определенный период времени.<br/>Метки:<br/>- _issue_type_ – тип ошибки, единственное значение – `optimistic_locks_invalidation`, подробнее инвалидация блокировок описана в разделе [Транзакции и запросы к {{ ydb-short-name }}](../../../concepts/transactions.md).

## Метрики GRPC API для топиков {#grpc_api_topics}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`grpc.topic.stream_read.commits`<br/>`RATE`, штуки | Количество коммитов метода `Ydb::TopicService::StreamRead`.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.bytes`<br/>`RATE`, штуки | Количество байт, прочитанных методом `Ydb::TopicService::StreamRead`.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.messages`<br/>`RATE`, штуки | Количество сообщений, прочитанных методом `Ydb::TopicService::StreamRead`.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.partition_session.errors`<br/>`RATE`, штуки | Количество ошибок при работе с партицией.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.partition_session.started`<br/>`RATE`, штуки | Количество сессий, запущенных в единицу времени.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.partition_session.stopped`<br/>`RATE`, штуки | Количество сессий, остановленных в единицу времени.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.partition_session.starting_count`<br/>`RATE`, штуки | Количество запускаемых сессий (то есть клиенту пришла команда о запуске сессии, но клиент еще не запустил сессию).<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.partition_session.stopping_count`<br/>`RATE`, штуки | Количество останавливаемых сессий.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_read.partition_session.count`<br/>`RATE`, штуки | Количество partition_session.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`grpc.topic.stream_write.bytes`<br/>`RATE`, байты | Количество байт, записанных методом `Ydb::TopicService::StreamWrite`.<br/>Метки:<br/>- _topic_ – название топика.
`grpc.topic.stream_write.uncommitted_bytes`<br/>`RATE`, байты | Количество байт, записанных методом `Ydb::TopicService::StreamWrite` в рамках ещё не закомиченных транзакций.<br/>Метки:<br/>- _topic_ – название топика.
`grpc.topic.stream_write.errors`<br/>`RATE`, штуки | Количество ошибок при вызове метода  `Ydb::TopicService::StreamWrite`.<br/>Метки:<br/>- _topic_ – название топика.
`grpc.topic.stream_write.messages`<br/>`RATE`, штуки | Количество сообщений, записанных методом `Ydb::TopicService::StreamWrite`.<br/>Метки:<br/>- _topic_ – название топика.
`grpc.topic.stream_write.uncommitted_messages`<br/>`RATE`, штуки | Количество сообщений, записанных методом `Ydb::TopicService::StreamWrite` в рамках ещё не закомиченных транзакций.<br/>Метки:<br/>- _topic_ – название топика.
`grpc.topic.stream_write.partition_throttled_milliseconds`<br/>`HIST_RATE`, штуки | Гистограммный счетчик. Интервалы заданы в миллисекундах. Показывает количество сообщений, ожидавших на квоте.<br/>Метки:<br/>- _topic_ – название топика.
`grpc.topic.stream_write.sessions_active_count`<br/>`GAUGE`, штуки | Количество открытых сессий записи.<br/>Метки:<br/>- _topic_ – название топика.
`grpc.topic.stream_write.sessions_created`<br/>`RATE`, штуки | Количество созданных сессий записи.<br/>Метки:<br/>- _topic_ – название топика.

## Метрики HTTP API {#http_api}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`api.http.data_streams.request.count`<br/>`RATE`, штуки  | Количество запросов по протоколу HTTP.<br/>Метки:<br/>- _method_ – название метода сервиса HTTP API, например `PutRecord`, `GetRecords`.<br/>- _topic_ – название топика.
`api.http.data_streams.request.bytes`<br/>`RATE`, байты  | Суммарный размер запросов по протоколу HTTP.<br/>Метки:<br/>- _method_ – название метода сервиса HTTP API, в данном случае только `PutRecord`.<br/>- _topic_ – название топика.
`api.http.data_streams.response.count`<br/>`RATE`, штуки  | Количество ответов по протоколу HTTP.<br/>Метки:<br/>- _method_ – название метода сервиса HTTP API, например `PutRecord`, `GetRecords`.<br/>- _topic_ – название топика.<br/>- _code_ – код ответа HTTP.
`api.http.data_streams.response.bytes`<br/>`RATE`, байты  | Суммарный размер ответов по протоколу HTTP.<br/>Метки:<br/>- _method_ – название метода сервиса HTTP API, в данном случае только `GetRecords`.<br/>- _topic_ – название топика.
`api.http.data_streams.response.duration_milliseconds`<br/>`HIST_RATE`, штуки  | Гистограммный счетчик. Интервалы заданы в миллисекундах. Показывает количество ответов, время выполнения которых попадает в определенный интервал.<br/>Метки:<br/>- _method_ – название метода сервиса HTTP API.<br/>- _topic_ – название топика.
`api.http.data_streams.get_records.messages`<br/>`RATE`, штуки | Количество сообщений, прочитанных методом `GetRecords`.<br/>Метки:<br/>- _topic_ – название топика.
`api.http.data_streams.put_record.messages`<br/>`RATE`, штуки | Количество сообщений, записанных методом `PutRecord` (всегда =1).<br/>Метки:<br/>- _topic_ – название топика.
`api.http.data_streams.put_records.failed_messages`<br/>`RATE`, штуки | Количество сообщений, отправленных методом `PutRecords`, которые не были записаны.<br/>Метки:<br/>- _topic_ – название топика.
`api.http.data_streams.put_records.successful_messages`<br/>`RATE`, штуки | Количество сообщений, отправленных методом `PutRecords`, которые были успешно записаны.<br/>Метки:<br/>- _topic_ – название топика.
`api.http.data_streams.put_records.total_messages`<br/>`RATE`, штуки | Количество сообщений, отправленных методом `PutRecords`.<br/>Метки:<br/>- _topic_ – название топика.

## Метрики Kafka API {#kafka_api}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`api.kafka.request.count`<br/>`RATE`, штуки  | Количество запросов по протоколу Kafka в единицу времени.<br/>Метки:<br/>- _method_ – название метода сервиса Kafka API, например `PRODUCE`, `SASL_HANDSHAKE`.
`api.kafka.request.bytes`<br/>`RATE`, байты  | Суммарный размер запросов по протоколу Kafka в единицу времени.<br/>Метки:<br/>- _method_ – название метода сервиса Kafka API, например `PRODUCE`, `SASL_HANDSHAKE`.
`api.kafka.response.count`<br/>`RATE`, штуки  | Количество ответов по протоколу Kafka в едининицу времени.<br/>Метки:<br/>- _method_ – название метода сервиса Kafka API, например `PRODUCE`, `SASL_HANDSHAKE`.<br/>- _error_code_ – код ответа Kafka.
`api.kafka.response.bytes`<br/>`RATE`, байты  | Суммарный размер ответов по протоколу Kafka в единицу времени.<br/>Метки:<br/>- _method_ – название метода сервиса Kafka API, например `PRODUCE`, `SASL_HANDSHAKE`.
`api.kafka.response.duration_milliseconds`<br/>`HIST_RATE`, штуки  | Гистограммный счетчик. Определяет набор интервалов в миллисекундах и для каждого из них показывает количество запросов с попадающим в этот интервал временем выполнения.<br/>Метки:<br/>- _method_ – название метода сервиса Kafka API.
`api.kafka.produce.failed_messages`<br/>`RATE`, штуки | Количество сообщений в единицу времени, отправленных методом `PRODUCE`, которые не были записаны.<br/>Метки:<br/>- _topic_ – название топика.
`api.kafka.produce.successful_messages`<br/>`RATE`, штуки | Количество сообщений в единицу времени, отправленных методом `PRODUCE`, которые были успешно записаны.<br/>Метки:<br/>- _topic_ – название топика.
`api.kafka.produce.total_messages`<br/>`RATE`, штуки | Количество сообщений в единицу времени, отправленных методом `PRODUCE`<br/>Метки:<br/>- _topic_ – название топика.

## Метрики сессий {#sessions}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`table.session.active_count`<br/>`IGAUGE`, штуки | Количество сессий, открытых клиентами в данный момент времени.
`table.session.closed_by_idle_count`<br/>`RATE`, штуки | Количество сессий, которые закрыты по инициативе сервера баз данных в определенный период времени из-за превышения времени, выделенного на существование неактивной сессии.

## Метрики обработки транзакций {#transactions}

Длительность выполнения транзакции можно анализировать с помощью гистограммного счетчика. Интервалы заданы в миллисекундах. График показывает количество транзакций, длительность которых попадает в определенный интервал времени.

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`table.transaction.total_duration_milliseconds`<br/>`HIST_RATE`, штуки | Количество транзакций определенной длительности выполнения на сервере и клиенте. Длительность выполнения – это время выполнения транзакции от момента явного или неявного открытия транзакции до момента фиксации изменений или отката. Включает время обработки транзакции на сервере и время на клиенте между отправкой разных запросов в одной транзакции.<br/>Метки:<br/>- _tx_kind_ – тип транзакции, возможные значения `read_only`, `read_write`, `write_only`, `pure`.
`table.transaction.server_duration_milliseconds`<br/>`HIST_RATE`, штуки | Количество транзакций определенной длительности выполнения на сервере. Длительность выполнения – это время выполнения запросов в транзакции на сервере. Не включет время ожидания на клиенте между отправкой отдельных запросов в одной транзакции.<br/>Метки:<br/> -_tx_kind_ – тип транзакции, возможные значения `read_only`, `read_write`, `write_only`, `pure`.
`table.transaction.client_duration_milliseconds`<br/>`HIST_RATE`, штуки | Количество транзакций определенной длительности выполнения на клиенте. Длительность выполнения – это время ожидания на клиенте между отправкой отдельных запросов в одной транзакции. Не включает время выполнения запросов на сервере.<br/>Метки:<br/>- _tx_kind_ – тип транзакции, возможные значения `read_only`, `read_write`, `write_only`, `pure`.

## Метрики обработки запросов {#queries}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`table.query.request.bytes`<br/>`RATE`, байты | Размер текста YQL-запросов и значений параметров к запросам, которые поступили в базу данных в определенный период времени.
`table.query.request.parameters_bytes`<br/>`RATE`, байты | Размер параметров к запросам, которые поступили в базу данных в определенный период времени.
`table.query.response.bytes`<br/>`RATE`, байты | Размер ответов, которые отправлены базой данных в определенный период времени.
`table.query.compilation.latency_milliseconds`<br/>`HIST_RATE`, штуки | Гистограммный счетчик. Интервалы заданы в миллисекундах. Показывает количество успешно выполненных запросов на компиляцию, длительность которых попадает в определенный интервал времени.
`table.query.compilation.active_count`<br/>`IGAUGE`, штуки | Количество компиляций, которые находятся в процессе выполнения в данный момент времени.
`table.query.compilation.count`<br/>`RATE`, штуки | Количество компиляций, которые успешно завершились в определенный период времени.
`table.query.compilation.errors`<br/>`RATE`, штуки | Количество компиляций, которые завершились с ошибкой в определенный период времени.
`table.query.compilation.cache_hits`<br/>`RATE`, штуки | Количество запросов в определенный период времени, для выполнения которых не потребовалось компилировать запрос, так как в кэше подготовленных запросов был созданный ранее план.
`table.query.compilation.cache_misses`<br/>`RATE`, штуки | Количество запросов в определенный период времени, для выполнения которых потребовалось компилировать запрос.
`table.query.execution.latency_milliseconds`<br/>`HIST_RATE`, штуки | Гистограммный счетчик. Интервалы заданы в миллисекундах. Показывает количество запросов, время выполнения которых попадает в определенный интервал.

## Метрики партиций таблиц {#datashards}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`table.datashard.row_count`<br/>`GAUGE`, штуки | Количество строк в таблицах базы данных.
`table.datashard.size_bytes`<br/>`GAUGE`, байты | Размер данных в таблицах базы.
`table.datashard.used_core_percents`<br/>`HIST_GAUGE`, % | Гистограммный счетчик. Интервалы заданы в процентах. Показывает количество партиций таблиц, которые используют вычислительные ресурсы в доле, попадающей в определенный интервал.
`table.datashard.read.rows`<br/>`RATE`, штуки | Количество строк, которые прочитаны всеми партициями всех таблиц в базе данных в определенный период времени.
`table.datashard.read.bytes`<br/>`RATE`, байты | Размер данных, которые прочитаны всеми партициями всех таблиц в базе в определенный период времени.
`table.datashard.write.rows`<br/>`RATE`, штуки | Количество строк, которые записаны всеми партициями всех таблиц в базе данных в определенный период времени.
`table.datashard.write.bytes`<br/>`RATE`, байты | Размер данных, которые записаны всеми партициями всех таблиц в базе в определенный период времени.
`table.datashard.scan.rows`<br/>`RATE`, штуки | Количество строк, которые прочитаны через вызовы gRPC API `StreamExecuteScanQuery` или `StreamReadTable` всеми партициями всех таблиц в базе данных в определенный период времени.
`table.datashard.scan.bytes`<br/>`RATE`, байты | Размер данных, которые прочитаны через вызов gRPC API `StreamExecuteScanQuery` или `StreamReadTable` всеми партициями всех таблиц в базе в определенный период времени.
`table.datashard.bulk_upsert.rows`<br/>`RATE`, штуки |  Количество строк, которые добавлены через вызов gRPC API `BulkUpsert` во все партиции всех таблиц в базе данных в определенный период времени.
`table.datashard.bulk_upsert.bytes`<br/>`RATE`, байты | Размер данных, которые добавлены через вызов gRPC API `BulkUpsert` во все партиции всех таблиц в базе в определенный период времени.
`table.datashard.erase.rows`<br/>`RATE`, штуки |  Количество строк, которые удалены в базе данных в определенный период времени.
`table.datashard.erase.bytes`<br/>`RATE`, байты | Размер данных, которые удалены в базе в определенный период времени.
`table.datashard.cache_hit.bytes`<br/>`RATE`, байты | Общий объем данных, успешно полученных из памяти (кэша). Больший объем данных, полученных из кэша, свидетельствует об эффективном использовании кэша без доступа к распределенному хранилищу.
`table.datashard.cache_miss.bytes`<br/>`RATE`, байты | Общий объем данных, которые были запрошены, но не найдены в памяти (кэше), и были прочитаны из распределенного хранилища.  Указывает на потенциальные области для оптимизации кэша.

## Метрики использования ресурсов (только для режима Dedicated) {#ydb_dedicated_resources}

Имя метрики<br/>Тип<br/>единицы измерения | Описание<br/>Метки
----- | -----
`resources.cpu.used_core_percents`<br/>`RATE`, % | Использование CPU. Значение `100` означает, что одно из ядер использовано на 100%. Значение может быть больше `100` для конфигураций с более чем 1 ядром.<br/>Метки:<br/>- _pool_ – вычислительный пул, возможные значения `user`, `system`, `batch`, `io`, `ic`.
`resources.cpu.limit_core_percents`<br/>`IGAUGE`, % | Доступный базе данных CPU в процентах. Например, для БД из трех нод по 4 ядра в `pool=user` в каждой ноде, значение этого сенсора будет равно `1200`.<br/>Метки:<br/>- _pool_ – вычислительный пул, возможные значения `user`, `system`, `batch`, `io`, `ic`.
`resources.memory.used_bytes`<br/>`IGAUGE`, байты |  Использованная нодами базы данных оперативная память.
`resources.memory.limit_bytes`<br/>`IGAUGE`, байты | Доступная нодам базы данных оперативная память.

## Метрики обработки запросов (только для режима Dedicated) {#ydb_dedicated_queries}

Имя метрики<br/>Тип<br/>единицы измерения | Описание<br/>Метки
----- | -----
`table.query.compilation.cache_evictions`<br/>`RATE`, штуки | Количество запросов, вытесненных из кэша подготовленных запросов в определенный период времени.
`table.query.compilation.cache_size_bytes`<br/>`IGAUGE`, байты | Размер кэша подготовленных запросов.
`table.query.compilation.cached_query_count`<br/>`IGAUGE`, штуки |  Размер кэша подготовленных запросов.

## Метрики топиков {#topics}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`topic.producers_count`<br/>`GAUGE`, штуки | Количество уникальных [источников](../../../concepts/topic#producer-id) топика.<br/>Метки:<br/>- _topic_ – название топика.
`topic.storage_bytes`<br/>`GAUGE`, байты | Размер топика в байтах.<br/>Метки:<br/>- _topic_ – название топика.
`topic.read.bytes`<br/>`RATE`, байты | Количество байт, прочитанных из топика.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.read.messages`<br/>`RATE`, штуки | Количество сообщений, прочитанных из топика.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.read.lag_messages`<br/>`RATE`, штуки | Суммарное по топику количество невычитанных данным читателем сообщений.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.read.lag_milliseconds`<br/>`HIST_RATE`, штуки | Гистограммный счетчик. Интервалы заданы в миллисекундах. Показывает количество сообщений, у которых разница между временем чтения и временем создания сообщения попадает в заданный интервал.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.write.bytes`<br/>`RATE`, байты | Размер записанных данных.<br/>Метки:<br/>- _topic_ – название топика.
`topic.write.uncommited_bytes`<br/>`RATE`, байты | Размер данных, записанных в рамках ещё не завершённых транзакций.<br/>Метки:<br/>- _topic_ — название топика.
`topic.write.uncompressed_bytes`<br/>`RATE`, байты | Размер разжатых записанных данных.<br/>Метки:<br/>- _topic_ – название топика.
`topic.write.messages`<br/>`RATE`, штуки | Количество записанных сообщений.<br/>Метки:<br/>- _topic_ – название топика.
`topic.write.uncommitted_messages`<br/>`RATE`, штуки | Количество сообщений, записанных в рамках ещё не завершённых транзакций.<br/>Метки:<br/>- _topic_ — название топика.
`topic.write.message_size_bytes`<br/>`HIST_RATE`, штуки | Гистограммный счетчик. Интервалы заданы в байтах. Показывает количество сообщений, размер которых соответствует границам интервала.<br/>Метки:<br/>- _topic_ – название топика.
`topic.write.lag_milliseconds`<br/>`HIST_RATE`, штуки | Гистограммный счетчик. Интервалы заданы в миллисекундах. Показывает количество сообщений, у которых разница между временем записи и временем создания сообщения попадает в заданный интервал.<br/>Метки:<br/>- _topic_ – название топика.

## Агрегированные метрики партиций топика {#topics_partitions}

В следующей таблице приведены агрегированные метрики партиций для топика. Максимальные и минимальные значения считаются по всем партициям заданного топика.

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`topic.partition.init_duration_milliseconds_max`<br/>`GAUGE`, миллисекунды | Максимальная задержка инициализации партиции.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.producers_count_max`<br/>`GAUGE`, штуки | Максимальное количество источников в партиции.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.storage_bytes_max`<br/>`GAUGE`, байты | Максимальный размер партиции в байтах.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.uptime_milliseconds_min`<br/>`GAUGE`, штуки | Минимальное время работы партиции после рестарта.<br/>В норме во время rolling restart-а `topic.partition.uptime_milliseconds_min` близко к 0, после окончания rolling restart-а значение `topic.partition.uptime_milliseconds_min` должно увеличиваться до бесконечности.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.total_count`<br/>`GAUGE`, штуки | Общее количество партиций в топике.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.alive_count`<br/>`GAUGE`, штуки | Количество партиций, отправляющих свои метрики.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.committed_end_to_end_lag_milliseconds_max`<br/>`GAUGE`, миллисекунды | Максимальная (по всем партициям) разница между текущим временем и временем создания последнего закомиченного сообщения.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.committed_lag_messages_max`<br/>`GAUGE`, штуки | Максимальная (по всем партициям) разница между последним оффсетом партиции и закомиченным оффсетом партиции.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.committed_read_lag_milliseconds_max`<br/>`GAUGE`, миллисекунды | Максимальная (по всем партициям) разница между текущим временем и временем записи последнего закомиченного сообщения.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.end_to_end_lag_milliseconds_max`<br/>`GAUGE`, миллисекунды | Разница между текущим временем и минимальным временем создания среди всех вычитанных за последнюю минуту сообщений во всех партициях.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.lag_messages_max`<br/>`GAUGE`, штуки | Максимальная разница (по всем партициям) последнего оффсета в партиции и последнего вычитанного оффсета.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.read.lag_milliseconds_max`<br/>`GAUGE`, миллисекунды | Разница между текущим временем и минимальным временем записи среди всех вычитанных за последнюю минуту сообщений во всех партициях.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.read.idle_milliseconds_max`<br/>`GAUGE`, миллисекунды | Максимальное время простоя (сколько времени не читали из партиции) по всем партициям.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.read.lag_milliseconds_max`<br/>`GAUGE`, миллисекунды | Максимальная разница между временем записи и временем создания среди всех вычитанных за последнюю минуту сообщений.<br/>Метки:<br/>- _topic_ – название топика.<br/>- _consumer_ – имя читателя.
`topic.partition.write.lag_milliseconds_max`<br/>`GAUGE`, миллисекунды | Максимальная разница между временем записи и временем создания среди всех записанных за последнюю минуту сообщений.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.write.speed_limit_bytes_per_second`<br/>`GAUGE`, байты в секунду | Квота на запись в байтах в секунду на одну партицию.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.write.throttled_nanoseconds_max`<br/>`GAUGE`, наносекунды | Максимальное время троттлинга записи (ожидания на квоте) по всем партициям. В пределе если `topic.partition.write.throttled_nanoseconds_maх` = 10^9, то это означает, что всю секунду ожидали на квоте.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.write.bytes_per_day_max`<br/>`GAUGE`, байты | Максимальное количество байт, записанное за последние сутки, по всем партициям.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.write.bytes_per_hour_max`<br/>`GAUGE`, байты | Максимальное количество байт, записанное за последний час, по всем партициям.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.write.bytes_per_minute_max`<br/>`GAUGE`, байты | Максимальное количество байт, записанное за последнюю минуту, по всем партициям.<br/>Метки:<br/>- _topic_ – название топика.
`topic.partition.write.idle_milliseconds_max`<br/>`GAUGE`, миллисекунды | Максимальное время простоя партиции на запись.<br/>Метки:<br/>- _topic_ – название топика.

## Метрики пулов ресурсов {#resource_pools}

Имя метрики<br/>Тип, единицы измерения | Описание<br/>Метки
----- | -----
`kqp.workload_manager.CpuQuotaManager.AverageLoadPercentage`<br/>`RATE`, штуки | Средняя загрузка базы данных, по этой метрики работает `DATABASE_LOAD_CPU_THRESHOLD`.
`kqp.workload_manager.InFlightLimit`<br/>`GAUGE`, штуки | Лимит на число одновременно работающих запросов.
`kqp.workload_manager.GlobalInFly`<br/>`GAUGE`, штуки | Текущее число одновременно работающих запросов. Отображаются только для пулов с включенным `CONCURRENT_QUERY_LIMIT` или `DATABASE_LOAD_CPU_THRESHOLD`.
`kqp.workload_manager.QueueSizeLimit`<br/>`GAUGE`, штуки | Размер очереди запросов, ожидающих выполнения.
`kqp.workload_manager.GlobalDelayedRequests`<br/>`GAUGE`, штуки | Количество запросов, ожидающих в очереди на выполнение. Отображаются только для пулов с включенным `CONCURRENT_QUERY_LIMIT` или `DATABASE_LOAD_CPU_THRESHOLD`.
