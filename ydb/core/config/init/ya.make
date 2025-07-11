LIBRARY()

SRCS(
    init.h
    init.cpp
    init_noop.cpp
    dummy.h
    dummy.cpp
)

PEERDIR(
    ydb/core/base
    ydb/core/driver_lib/cli_base
    ydb/core/driver_lib/cli_config_base
    ydb/core/protos
    ydb/library/yaml_config
    yql/essentials/minikql
    yql/essentials/public/udf
    ydb/public/lib/deprecated/kicli
    ydb/public/sdk/cpp/src/client/config
    ydb/public/sdk/cpp/src/client/discovery
    ydb/public/sdk/cpp/src/client/driver
)

GENERATE_ENUM_SERIALIZATION(init.h)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(
    ut
)

