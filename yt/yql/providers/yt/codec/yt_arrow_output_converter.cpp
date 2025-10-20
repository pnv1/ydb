#include "yt_arrow_output_converter.h"

#include <yql/essentials/minikql/mkql_type_builder.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/public/udf/arrow/defs.h>
#include <yql/essentials/utils/yql_panic.h>

#include <arrow/compute/cast.h>

namespace NYql {

class TBasicOutputConverter : public IYtOutputColumnConverter {
public:
    TBasicOutputConverter(std::shared_ptr<arrow20::DataType> arrowType)
        : ArrowType_(arrowType)
    {}

    std::shared_ptr<arrow20::ArrayData> Convert(std::shared_ptr<arrow20::ArrayData> block) override {
        YQL_ENSURE(ArrowType_->Equals(block->type), "block type differs from expected arrow output type");
        return block;
    }

    std::shared_ptr<arrow20::DataType> GetOutputType() override {
        return ArrowType_;
    }

private:
    std::shared_ptr<arrow20::DataType> ArrowType_;
};

class TBoolOutputConverter : public IYtOutputColumnConverter {
public:
    TBoolOutputConverter(arrow20::MemoryPool* pool)
        : ExecContext_(pool)
    {}

    std::shared_ptr<arrow20::ArrayData> Convert(std::shared_ptr<arrow20::ArrayData> block) override {
        YQL_ENSURE(block->type->Equals(arrow20::uint8()));
        auto convertedDatum = ARROW_RESULT(arrow20::compute::Cast(block, arrow20::boolean(), arrow20::compute::CastOptions::Safe(), &ExecContext_));
        return convertedDatum.array();
    }

    std::shared_ptr<arrow20::DataType> GetOutputType() override {
        return arrow20::boolean();
    }

private:
    arrow20::compute::ExecContext ExecContext_;
};

IYtOutputColumnConverter::TPtr MakeYtOutputColumnConverter(NKikimr::NMiniKQL::TType* type, arrow20::MemoryPool* pool) {
    std::shared_ptr<arrow20::DataType> arrowType;
    YQL_ENSURE(ConvertArrowOutputType(type, arrowType), "unsupported arrow output type");

    // only data and optional data types are supported at the moment
    // TODO: support complex types

    if (type->IsOptional()) {
        type = AS_TYPE(NKikimr::NMiniKQL::TOptionalType, type)->GetItemType();
    }

    if (AS_TYPE(NKikimr::NMiniKQL::TDataType, type)->GetDataSlot() == NUdf::EDataSlot::Bool) {
        return std::make_unique<TBoolOutputConverter>(pool);
    } else {
        return std::make_unique<TBasicOutputConverter>(std::move(arrowType));
    }
}

}
