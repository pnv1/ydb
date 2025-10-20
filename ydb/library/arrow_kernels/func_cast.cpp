#include "func_cast.h"

#include <util/system/yassert.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include <contrib/libs/apache/arrow_next/cpp/src/arrow/compute/kernel.h>
#include <contrib/libs/apache/arrow_next/cpp/src/arrow/compute/kernels/scalar_cast_internal.h>
#include <contrib/libs/apache/arrow_next/cpp/src/arrow/util/time.h>
#pragma clang diagnostic pop

#include <unordered_map>

namespace cp = ::arrow20::compute;

namespace NKikimr::NKernels {

namespace {

std::shared_ptr<cp::internal::CastFunction> GetYdbTimestampCast() {
    auto func = std::make_shared<cp::internal::CastFunction>("ydb_cast_timestamp", ::arrow20::Type::TIMESTAMP);
    cp::internal::AddSimpleCast<arrow20::UInt32Type, arrow20::TimestampType>(
        cp::InputType(arrow20::Type::UINT32),
        cp::internal::kOutputTargetType,
        func.get()
    );
    return func;
}

std::vector<std::shared_ptr<cp::internal::CastFunction>> GetYdbTemporalCasts() {
    std::vector<std::shared_ptr<cp::internal::CastFunction>> functions;
    functions.push_back(GetYdbTimestampCast());
    return functions;
}

std::unordered_map<int, std::shared_ptr<cp::internal::CastFunction>> ydbCastTable;
std::once_flag ydbCastTableInitialized;

void AddCastFunctions(const std::vector<std::shared_ptr<cp::internal::CastFunction>>& funcs) {
    for (const auto& func : funcs) {
        ydbCastTable[static_cast<int>(func->out_type_id())] = func;
    }
}

void InitYdbCastTable() {
    AddCastFunctions(GetYdbTemporalCasts());
}

void EnsureInitYdbCastTable() {
    std::call_once(ydbCastTableInitialized, InitYdbCastTable);
}

// Private version of GetCastFunction with better error reporting
// if the input type is known.
::arrow20::Result<std::shared_ptr<cp::internal::CastFunction>> GetYdbCastFunctionInternal(
    const std::shared_ptr<::arrow20::DataType>& to_type, const::arrow20::DataType* from_type = nullptr) {
    EnsureInitYdbCastTable();
    auto it = ydbCastTable.find(static_cast<int>(to_type->id()));
    if (it == ydbCastTable.end()) {
        auto res = cp::internal::GetCastFunction(*to_type);
        if (!res.ok()) {
            if (from_type != nullptr) {
               return ::arrow20::Status::NotImplemented("Unsupported cast from ", *from_type, " to ",
                                              *to_type,
                                              " (no available cast function for target type)");
            } else {
                return ::arrow20::Status::NotImplemented("Unsupported cast to ", *to_type,
                                            " (no available cast function for target type)");
            }
        }
        return std::move(res).ValueUnsafe();
    }
    return it->second;
}

} // namespace

static const cp::FunctionDoc ydbCastDoc{"YDB special cast function. Uses Arrow's cast and add casting support for some types."
                            "Cast values to another data type",
                           ("Behavior when values wouldn't fit in the target type\n"
                            "can be controlled through CastOptions."),
                           {"input"},
                           "CastOptions"};

YdbCastMetaFunction::YdbCastMetaFunction()
    : ::arrow20::compute::MetaFunction("ydb.cast", ::arrow20::compute::Arity::Unary(), ydbCastDoc)
    {}

::arrow20::Result<const cp::CastOptions*> YdbCastMetaFunction::ValidateOptions(const cp::FunctionOptions* options) const {
    auto cast_options = static_cast<const cp::CastOptions*>(options);

    if (cast_options == nullptr || cast_options->to_type == nullptr) {
      return ::arrow20::Status::Invalid(
          "Cast requires that options be passed with "
          "the to_type populated");
    }

    return cast_options;
}

::arrow20::Result<::arrow20::Datum> YdbCastMetaFunction::ExecuteImpl(const std::vector<::arrow20::Datum>& args,
                            const cp::FunctionOptions* options,
                            cp::ExecContext* ctx) const
{
    auto&& optsResult = ValidateOptions(options);
    if (!optsResult.ok()) {
        return optsResult.status();
    }
    auto cast_options = std::move(optsResult).ValueUnsafe();
    if (args[0].type()->Equals(*cast_options->to_type)) {
      return args[0];
    }
    auto&& castFuncResult = GetYdbCastFunctionInternal(cast_options->to_type.GetSharedPtr(), args[0].type()->GetSharedPtr().get());
    if (!castFuncResult.ok()) {
        return castFuncResult.status();
    }
    std::shared_ptr<cp::internal::CastFunction> castFunc = std::move(castFuncResult).ValueUnsafe();
    return castFunc->Execute(args, options, ctx);
}

}


namespace arrow20::compute::internal {

template <>
struct CastFunctor<TimestampType, UInt32Type> {
    static Status Exec(KernelContext* /*ctx*/, const ExecSpan& batch, ExecResult* out) {
        if (batch.num_values() == 0) {
            return ::arrow20::Status::IndexError("Cast from uint32 to timestamp received empty batch.");
        }
        Y_ABORT_UNLESS(batch[0].is_array(), "Cast from uint32 to timestamp expected ARRAY as input.");

        const auto& out_type = checked_cast<const ::arrow20::TimestampType&>(*out->type());
        // get conversion MICROSECONDS -> unit
        auto conversion = ::arrow20::util::GetTimestampConversion(::arrow20::TimeUnit::MICRO, out_type.unit());
        Y_ABORT_UNLESS(conversion.first == ::arrow20::util::MULTIPLY, "Cast from uint32 to timestamp failed because timestamp unit is greater than seconds.");

        auto input = batch[0].array;
        auto output = out->array_span_mutable();
        auto in_data = input.GetValues<uint32_t>(1);
        auto out_data = output->buffers[1].mutable_data_as<int64_t>();

        for (int64_t i = 0; i < input.length; i++) {
            out_data[i] = static_cast<int64_t>(in_data[i] * conversion.second);
        }
        return ::arrow20::Status::OK();
    }
};

} // namespace arrow20::compute::internal
