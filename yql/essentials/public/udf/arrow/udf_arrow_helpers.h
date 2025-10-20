#pragma once
#include <yql/essentials/public/udf/udf_type_builder.h>
#include <yql/essentials/public/udf/udf_value.h>
#include <yql/essentials/public/udf/udf_helpers.h>
#include <yql/essentials/public/udf/udf_data_type.h>
#include <yql/essentials/public/udf/udf_type_inspection.h>

#include "defs.h"
#include "util.h"
#include "args_dechunker.h"
#include "block_reader.h"
#include "block_builder.h"
#include "memory_pool.h"

#include <arrow/array/array_base.h>
#include <arrow/array/util.h>
#include <arrow/c/bridge.h>
#include <arrow/chunked_array.h>
#include <arrow/compute/kernel.h>
#include <arrow/compute/exec_internal.h>
#include <arrow/util/bitmap_ops.h>

namespace NYql {
namespace NUdf {

using TExec = arrow20::Status (*)(arrow20::compute::KernelContext*, const arrow20::compute::ExecSpan&, arrow20::compute::ExecResult*);

class TUdfKernelState: public arrow20::compute::KernelState {
public:
    TUdfKernelState(const TVector<const TType*>& argTypes, const TType* outputType, bool onlyScalars, const ITypeInfoHelper* typeInfoHelper, const IValueBuilder* valueBuilder)
        : ArgTypes_(argTypes)
        , OutputType_(outputType)
        , OnlyScalars_(onlyScalars)
        , TypeInfoHelper_(typeInfoHelper)
        , ValueBuilder_(valueBuilder)
    {
        Readers_.resize(ArgTypes_.size());
    }

    IBlockReader& GetReader(ui32 index) {
        if (!Readers_[index]) {
            Readers_[index] = MakeBlockReader(*TypeInfoHelper_, ArgTypes_[index]);
        }

        return *Readers_[index];
    }

    IArrayBuilder& GetArrayBuilder() {
        Y_ENSURE(!OnlyScalars_);
        if (!ArrayBuilder_) {
            ArrayBuilder_ = MakeArrayBuilder(*TypeInfoHelper_, OutputType_, *GetYqlMemoryPool(), TypeInfoHelper_->GetMaxBlockLength(OutputType_), &ValueBuilder_->GetPgBuilder());
        }

        return *ArrayBuilder_;
    }

    IScalarBuilder& GetScalarBuilder() {
        Y_ENSURE(OnlyScalars_);
        if (!ScalarBuilder_) {
            ScalarBuilder_ = MakeScalarBuilder(*TypeInfoHelper_, OutputType_);
        }

        return *ScalarBuilder_;
    }

    const IValueBuilder& GetValueBuilder() {
        Y_ENSURE(ValueBuilder_);
        return *ValueBuilder_;
    }

private:
    const TVector<const TType*> ArgTypes_;
    const TType* OutputType_;
    const bool OnlyScalars_;
    const ITypeInfoHelper* TypeInfoHelper_;
    const IValueBuilder* ValueBuilder_;
    TVector<std::unique_ptr<IBlockReader>> Readers_;
    std::unique_ptr<IArrayBuilder> ArrayBuilder_;
    std::unique_ptr<IScalarBuilder> ScalarBuilder_;
};

class TSimpleArrowUdfImpl: public TBoxedValue {
public:
    TSimpleArrowUdfImpl(const TVector<const TType*> argBlockTypes, const TType* outputType, bool onlyScalars,
                        TExec exec, IFunctionTypeInfoBuilder& builder, const TString& name,
                        arrow20::compute::NullHandling::type nullHandling)
        : OnlyScalars_(onlyScalars)
        , Exec_(exec)
        , Pos_(GetSourcePosition(builder))
        , Name_(name)
        , OutputType_(outputType)
        , NullDatum_(arrow20::Datum(std::make_shared<arrow20::NullScalar>()))
    {
        TypeInfoHelper_ = builder.TypeInfoHelper();
        Kernel_.null_handling = nullHandling;
        Kernel_.exec = Exec_;
        std::vector<arrow20::compute::InputType> inTypes;
        for (const auto& blockType : argBlockTypes) {
            TBlockTypeInspector blockInspector(*TypeInfoHelper_, blockType);
            Y_ENSURE(blockInspector);
            ArgTypes_.push_back(blockInspector.GetItemType());

            auto arrowTypeHandle = TypeInfoHelper_->MakeArrowType(blockInspector.GetItemType());
            Y_ENSURE(arrowTypeHandle);
            ArrowSchema s;
            arrowTypeHandle->Export(&s);
            auto type = ARROW_RESULT(arrow20::ImportType(&s));
            ArgArrowTypes_.emplace_back(type);

            inTypes.emplace_back(type);
            ArgsValuesDescr_.emplace_back(type);
        }

        ReturnArrowTypeHandle_ = TypeInfoHelper_->MakeArrowType(outputType);
        Y_ENSURE(ReturnArrowTypeHandle_);

        ArrowSchema s;
        ReturnArrowTypeHandle_->Export(&s);
        auto outType = arrow20::compute::OutputType(ARROW_RESULT(arrow20::ImportType(&s)));

        Kernel_.signature = arrow20::compute::KernelSignature::Make(std::move(inTypes), std::move(outType));
    }

    TUnboxedValue Run(const IValueBuilder* valueBuilder, const TUnboxedValuePod* args) const final {
        try {
            TVector<arrow20::Datum> argDatums(ArgArrowTypes_.size());
            for (ui32 i = 0; i < ArgArrowTypes_.size(); ++i) {
                bool isScalar;
                ui64 length;
                // If no value is given to the UDF, pass the Null datum as an
                // optional argument value to the fixed-arg kernel.
                // XXX: Use bool operator for TUnboxedValuePod object instead
                // of its HasValue method due to just(null) semantics.
                if (!args[i]) {
                    argDatums[i] = NullDatum_;
                    continue;
                }
                ui32 chunkCount = valueBuilder->GetArrowBlockChunks(args[i], isScalar, length);
                if (isScalar) {
                    ArrowArray a;
                    valueBuilder->ExportArrowBlock(args[i], 0, &a);
                    auto arr = ARROW_RESULT(arrow20::ImportArray(&a, ArgArrowTypes_[i]));
                    auto scalar = ARROW_RESULT(arr->GetScalar(0));
                    argDatums[i] = scalar;
                } else {
                    TVector<std::shared_ptr<arrow20::Array>> imported(chunkCount);
                    for (ui32 k = 0; k < chunkCount; ++k) {
                        ArrowArray a;
                        valueBuilder->ExportArrowBlock(args[i], k, &a);
                        auto arr = ARROW_RESULT(arrow20::ImportArray(&a, ArgArrowTypes_[i]));
                        imported[k] = arr;
                    }

                    if (chunkCount == 1) {
                        argDatums[i] = imported.front();
                    } else {
                        argDatums[i] = ARROW_RESULT(arrow20::ChunkedArray::Make(std::move(imported), ArgArrowTypes_[i]));
                    }
                }
            }

            TUdfKernelState kernelState(ArgTypes_, OutputType_, OnlyScalars_, TypeInfoHelper_.Get(), valueBuilder);
            arrow20::compute::ExecContext execContext(GetYqlMemoryPool());
            arrow20::compute::KernelContext kernelContext(&execContext);
            kernelContext.SetState(&kernelState);

            arrow20::Datum res;
            if (OnlyScalars_) {
                auto executor = arrow20::compute::detail::KernelExecutor::MakeScalar();
                ARROW_OK(executor->Init(&kernelContext, {&Kernel_, ArgsValuesDescr_, nullptr}));

                auto listener = std::make_shared<arrow20::compute::detail::DatumAccumulator>();
                auto batch = arrow20::compute::ExecBatch(argDatums, argDatums.empty() ? 0 : argDatums[0].length());
                ARROW_OK(executor->Execute(batch, listener.get()));
                res = executor->WrapResults(argDatums, listener->values());
            } else {
                TArgsDechunker dechunker(std::move(argDatums));
                std::vector<arrow20::Datum> chunk;
                TVector<std::shared_ptr<arrow20::ArrayData>> arrays;

                while (dechunker.Next(chunk)) {
                    auto executor = arrow20::compute::detail::KernelExecutor::MakeScalar();
                    ARROW_OK(executor->Init(&kernelContext, {&Kernel_, ArgsValuesDescr_, nullptr}));

                    arrow20::compute::detail::DatumAccumulator listener;
                    auto chunkBatch = arrow20::compute::ExecBatch(chunk, chunk.empty() ? 0 : chunk[0].length());
                    ARROW_OK(executor->Execute(chunkBatch, &listener));
                    auto output = executor->WrapResults(chunk, listener.values());

                    ForEachArrayData(output, [&](const auto& arr) { arrays.push_back(arr); });
                }

                res = MakeArray(arrays);
            }

            if (OnlyScalars_) {
                auto arr = ARROW_RESULT(arrow20::MakeArrayFromScalar(*res.scalar(), 1));
                ArrowArray a;
                ARROW_OK(arrow20::ExportArray(*arr, &a));
                return valueBuilder->ImportArrowBlock(&a, 1, true, *ReturnArrowTypeHandle_);
            } else {
                TVector<ArrowArray> a;
                if (res.is_array()) {
                    a.resize(1);
                    ARROW_OK(arrow20::ExportArray(*res.make_array(), &a[0]));
                } else {
                    Y_ENSURE(res.is_arraylike());
                    a.resize(res.chunks().size());
                    for (ui32 i = 0; i < res.chunks().size(); ++i) {
                        ARROW_OK(arrow20::ExportArray(*res.chunks()[i], &a[i]));
                    }
                }

                return valueBuilder->ImportArrowBlock(a.data(), a.size(), false, *ReturnArrowTypeHandle_);
            }
        } catch (const std::exception& ex) {
            TStringBuilder sb;
            APPEND_SOURCE_LOCATION(sb, valueBuilder, Pos_)
            sb << ex.what();
            UdfTerminate(sb.c_str());
        }
    }

private:
    const bool OnlyScalars_;
    const TExec Exec_;
    TSourcePosition Pos_;
    const TString Name_;
    const TType* OutputType_;
    ITypeInfoHelper::TPtr TypeInfoHelper_;

    TVector<std::shared_ptr<arrow20::DataType>> ArgArrowTypes_;
    IArrowType::TPtr ReturnArrowTypeHandle_;

    arrow20::compute::ScalarKernel Kernel_;
    std::vector<arrow20::TypeHolder> ArgsValuesDescr_;
    TVector<const TType*> ArgTypes_;
    const arrow20::Datum NullDatum_;
};

inline void SetCallableArgumentAttributes(IFunctionArgTypesBuilder& argsBuilder,
                                          const TCallableTypeInspector& callableInspector, const ui32 index) {
    if (callableInspector.GetArgumentName(index).Size() > 0) {
        argsBuilder.Name(callableInspector.GetArgumentName(index));
    }
    if (callableInspector.GetArgumentFlags(index) != 0) {
        argsBuilder.Flags(callableInspector.GetArgumentFlags(index));
    }
}

inline void PrepareSimpleArrowUdf(IFunctionTypeInfoBuilder& builder, TType* signature, TType* userType, TExec exec, bool typesOnly,
                                  const TString& name, arrow20::compute::NullHandling::type nullHandling = arrow20::compute::NullHandling::type::COMPUTED_NO_PREALLOCATE) {
    auto typeInfoHelper = builder.TypeInfoHelper();
    TCallableTypeInspector callableInspector(*typeInfoHelper, signature);
    Y_ENSURE(callableInspector);
    Y_ENSURE(callableInspector.GetArgsCount() > 0);
    TTupleTypeInspector userTypeInspector(*typeInfoHelper, userType);
    Y_ENSURE(userTypeInspector);
    Y_ENSURE(userTypeInspector.GetElementsCount() == 3);
    TTupleTypeInspector argsInspector(*typeInfoHelper, userTypeInspector.GetElementType(0));
    Y_ENSURE(argsInspector);
    Y_ENSURE(argsInspector.GetElementsCount() <= callableInspector.GetArgsCount());
    const ui32 omitted = callableInspector.GetArgsCount() - argsInspector.GetElementsCount();
    Y_ENSURE(omitted <= callableInspector.GetOptionalArgsCount());

    bool hasBlocks = false;
    bool onlyScalars = true;
    for (ui32 i = 0; i < argsInspector.GetElementsCount(); ++i) {
        TBlockTypeInspector blockInspector(*typeInfoHelper, argsInspector.GetElementType(i));
        if (blockInspector) {
            if (i == 0) {
                hasBlocks = true;
            } else {
                Y_ENSURE(hasBlocks);
            }

            onlyScalars = onlyScalars && blockInspector.IsScalar();
        }
    }

    builder.SupportsBlocks();
    builder.UserType(userType);
    Y_ENSURE(hasBlocks);

    TVector<const TType*> argBlockTypes;
    auto argsBuilder = builder.Args(callableInspector.GetArgsCount());
    for (ui32 i = 0; i < argsInspector.GetElementsCount(); ++i) {
        TBlockTypeInspector blockInspector(*typeInfoHelper, argsInspector.GetElementType(i));
        auto type = callableInspector.GetArgType(i);
        auto argBlockType = builder.Block(blockInspector.IsScalar())->Item(type).Build();
        argsBuilder->Add(argBlockType);
        SetCallableArgumentAttributes(*argsBuilder, callableInspector, i);
        argBlockTypes.emplace_back(argBlockType);
    }

    // XXX: Append the Block types for the omitted arguments to preserve the
    // fixed-arg kernel signature. Unlikely to the required arguments,
    // initialized above, the type of the omitted argument has to be passed to
    // the specialized UDF signature builder (i.e. argsBuilder) as an original
    // TOptional parameter type. At the same time, all of omitted arguments have
    // to be substituted with Null datums, so all the original types of the
    // optional parameters are wrapped type with the Block type with the Scalar
    // shape in the UDFKernel signature (i.e. argBlockTypes).
    for (ui32 i = argsInspector.GetElementsCount(); i < callableInspector.GetArgsCount(); i++) {
        auto optType = callableInspector.GetArgType(i);
        argsBuilder->Add(optType);
        SetCallableArgumentAttributes(*argsBuilder, callableInspector, i);
        argBlockTypes.emplace_back(builder.Block(true)->Item(optType).Build());
    }

    builder.Returns(builder.Block(onlyScalars)->Item(callableInspector.GetReturnType()).Build());
    // XXX: Only the omitted parameters should be specified as optional
    // arguments in this context.
    if (omitted) {
        builder.OptionalArgs(omitted);
    }

    if (callableInspector.GetPayload().Size() > 0) {
        builder.PayloadImpl(callableInspector.GetPayload());
    }

    if (!typesOnly) {
        builder.Implementation(static_cast<IBoxedValue*>(new TSimpleArrowUdfImpl(argBlockTypes, callableInspector.GetReturnType(),
                                                       onlyScalars, exec, builder, name, nullHandling)));
    }
}

template <typename TBuilder>
TBuilder* CastToArrayBuilderImpl(IArrayBuilder& builder) {
    static_assert(std::is_base_of_v<IArrayBuilder, TBuilder>);

    auto* builderImpl = dynamic_cast<TBuilder*>(&builder);
    Y_ENSURE(builderImpl, TStringBuilder() << "Got " << TypeName(builder) << " as ArrayBuilder");
    return builderImpl;
}

template <typename TScalarBuilderImpl>
TScalarBuilderImpl* CastToScalarBuilderImpl(IScalarBuilder& builder) {
    static_assert(std::is_base_of_v<IScalarBuilder, TScalarBuilderImpl>);

    auto* builderImpl = dynamic_cast<TScalarBuilderImpl*>(&builder);
    Y_ENSURE(builderImpl, TStringBuilder() << "Got " << TypeName(builder) << " as ScalarBuilder");
    return builderImpl;
}

template <typename TReader>
TReader* CastToBlockReaderImpl(IBlockReader& reader) {
    static_assert(std::is_base_of_v<IBlockReader, TReader>);

    auto* readerImpl = dynamic_cast<TReader*>(&reader);
    Y_ENSURE(readerImpl, TStringBuilder() << "Got " << TypeName(reader) << " as BlockReader");
    return readerImpl;
}

template <typename TDerived, typename TReader = IBlockReader, typename TArrayBuilderImpl = IArrayBuilder, typename TScalarBuilderImpl = IScalarBuilder>
struct TUnaryKernelExec {
    static arrow20::Status Do(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
        auto& state = dynamic_cast<TUdfKernelState&>(*ctx->state());
        auto& reader = state.GetReader(0);
        auto* readerImpl = CastToBlockReaderImpl<TReader>(reader);

        const auto& arg = batch.values[0];
        if (arg.is_scalar()) {
            auto& builder = state.GetScalarBuilder();
            auto* builderImpl = CastToScalarBuilderImpl<TScalarBuilderImpl>(builder);

            auto item = readerImpl->GetScalarItem(*arg.scalar);
            TDerived::Process(&state.GetValueBuilder(), item, [&](TBlockItem out) {
                auto datum = builderImpl->Build(out); res->value = datum.array();
            });
        } else {
            auto& array = arg.array;
            auto& builder = state.GetArrayBuilder();
            auto* builderImpl = CastToArrayBuilderImpl<TArrayBuilderImpl>(builder);

            size_t maxBlockLength = builderImpl->MaxLength();
            Y_ENSURE(maxBlockLength > 0);
            TVector<std::shared_ptr<arrow20::ArrayData>> outputArrays;
            for (int64_t i = 0; i < array.length;) {
                for (size_t j = 0; j < maxBlockLength && i < array.length; ++j, ++i) {
                    auto item = readerImpl->GetItem(*array.ToArrayData(), i);
                    TDerived::Process(&state.GetValueBuilder(), item, [&](TBlockItem out) {
                        builderImpl->Add(out);
                    });
                }
                auto outputDatum = builderImpl->Build(false);
                ForEachArrayData(outputDatum, [&](const auto& arr) { outputArrays.push_back(arr); });
            }

            auto resultDatum = MakeArray(outputArrays);
            res->value = resultDatum.array();
        }

        return arrow20::Status::OK();
    }
};

template <typename TDerived, typename TReader1 = IBlockReader, typename TReader2 = IBlockReader, typename TArrayBuilderImpl = IArrayBuilder, typename TScalarBuilderImpl = IScalarBuilder>
struct TBinaryKernelExec {
    static arrow20::Status Do(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
        auto& state = dynamic_cast<TUdfKernelState&>(*ctx->state());

        auto& reader1 = state.GetReader(0);
        auto* reader1Impl = CastToBlockReaderImpl<TReader1>(reader1);

        auto& reader2 = state.GetReader(1);
        auto* reader2Impl = CastToBlockReaderImpl<TReader2>(reader2);

        const auto& arg1 = batch.values[0];
        const auto& arg2 = batch.values[1];
        if (arg1.is_scalar() && arg2.is_scalar()) {
            auto& builder = state.GetScalarBuilder();
            auto* builderImpl = CastToScalarBuilderImpl<TScalarBuilderImpl>(builder);

            auto item1 = reader1Impl->GetScalarItem(*arg1.scalar);
            auto item2 = reader2Impl->GetScalarItem(*arg2.scalar);

            TDerived::Process(&state.GetValueBuilder(), item1, item2, [&](TBlockItem out) {
                auto datum = builderImpl->Build(out); res->value = datum.array();
            });
        } else if (arg1.is_scalar() && arg2.is_array()) {
            auto item1 = reader1Impl->GetScalarItem(*arg1.scalar);
            auto& array2 = arg2.array;
            auto& builder = state.GetArrayBuilder();
            auto* builderImpl = CastToArrayBuilderImpl<TArrayBuilderImpl>(builder);

            size_t maxBlockLength = builder.MaxLength();
            Y_ENSURE(maxBlockLength > 0);
            TVector<std::shared_ptr<arrow20::ArrayData>> outputArrays;
            for (int64_t i = 0; i < array2.length;) {
                for (size_t j = 0; j < maxBlockLength && i < array2.length; ++j, ++i) {
                    auto item2 = reader2Impl->GetItem(*array2.ToArrayData(), i);
                    TDerived::Process(&state.GetValueBuilder(), item1, item2, [&](TBlockItem out) {
                        builderImpl->Add(out);
                    });
                }
                auto outputDatum = builder.Build(false);
                ForEachArrayData(outputDatum, [&](const auto& arr) { outputArrays.push_back(arr); });
            }

            auto resultDatum = MakeArray(outputArrays);
            res->value = resultDatum.array();
        } else if (arg1.is_array() && arg2.is_scalar()) {
            auto& array1 = arg1.array;
            auto item2 = reader2Impl->GetScalarItem(*arg2.scalar);
            auto& builder = state.GetArrayBuilder();
            auto* builderImpl = CastToArrayBuilderImpl<TArrayBuilderImpl>(builder);

            size_t maxBlockLength = builder.MaxLength();
            Y_ENSURE(maxBlockLength > 0);
            TVector<std::shared_ptr<arrow20::ArrayData>> outputArrays;
            for (int64_t i = 0; i < array1.length;) {
                for (size_t j = 0; j < maxBlockLength && i < array1.length; ++j, ++i) {
                    auto item1 = reader1Impl->GetItem(*array1.ToArrayData(), i);
                    TDerived::Process(&state.GetValueBuilder(), item1, item2, [&](TBlockItem out) {
                        builderImpl->Add(out);
                    });
                }
                auto outputDatum = builder.Build(false);
                ForEachArrayData(outputDatum, [&](const auto& arr) { outputArrays.push_back(arr); });
            }

            auto resultDatum = MakeArray(outputArrays);
            res->value = resultDatum.array();
        } else {
            Y_ENSURE(arg1.is_array() && arg2.is_array());
            auto& array1 = arg1.array;
            auto& array2 = arg2.array;
            auto& builder = state.GetArrayBuilder();
            auto* builderImpl = CastToArrayBuilderImpl<TArrayBuilderImpl>(builder);

            size_t maxBlockLength = builder.MaxLength();
            Y_ENSURE(maxBlockLength > 0);
            TVector<std::shared_ptr<arrow20::ArrayData>> outputArrays;
            Y_ENSURE(array1.length == array2.length);
            for (int64_t i = 0; i < array1.length;) {
                for (size_t j = 0; j < maxBlockLength && i < array1.length; ++j, ++i) {
                    auto item1 = reader1Impl->GetItem(*array1.ToArrayData(), i);
                    auto item2 = reader2Impl->GetItem(*array2.ToArrayData(), i);
                    TDerived::Process(&state.GetValueBuilder(), item1, item2, [&](TBlockItem out) {
                        builderImpl->Add(out);
                    });
                }
                auto outputDatum = builder.Build(false);
                ForEachArrayData(outputDatum, [&](const auto& arr) { outputArrays.push_back(arr); });
            }

            auto resultDatum = MakeArray(outputArrays);
            res->value = resultDatum.array();
        }

        return arrow20::Status::OK();
    }
};

template <typename TDerived, size_t Argc, typename TArrayBuilderImpl = IArrayBuilder, typename TScalarBuilderImpl = IScalarBuilder>
struct TGenericKernelExec {
    static arrow20::Status Do(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
        auto& state = dynamic_cast<TUdfKernelState&>(*ctx->state());
        Y_ENSURE(batch.num_values() == Argc);
        // XXX: Since Arrow arrays ought to have the valid length value, use
        // this constant to check whether all the arrays in the given batch have
        // the same length and also as an indicator whether there is no array
        // arguments in the given batch.
        int64_t alength = arrow20::Datum::kUnknownLength;
        // XXX: Allocate fixed-size buffer to pass the parameters into the
        // Process routine (stored into BlockItem), since only the content
        // of the particular cells will be updated in the main "process" loop.
        std::array<TBlockItem, Argc> args;
        const TBlockItem items(args.data());
        // XXX: Introduce scalar/array mapping to avoid excess scalar copy ops
        // in the main "process" loop.
        std::array<bool, Argc> needUpdate;
        needUpdate.fill(false);

        for (size_t k = 0; k < Argc; k++) {
            auto& arg = batch[k];
            Y_ENSURE(arg.is_scalar() || arg.is_array());
            if (arg.is_scalar()) {
                continue;
            }
            if (alength == arrow20::Datum::kUnknownLength) {
                alength = arg.length();
            } else {
                Y_ENSURE(arg.length() == alength);
            }
            needUpdate[k] = true;
        }
        // Specialize the case, when all given arguments are scalar.
        if (alength == arrow20::Datum::kUnknownLength) {
            auto& builder = state.GetScalarBuilder();
            auto* builderImpl = CastToScalarBuilderImpl<TScalarBuilderImpl>(builder);

            for (size_t k = 0; k < Argc; k++) {
                auto& reader = state.GetReader(k);
                args[k] = reader.GetScalarItem(*batch[k].scalar);
            }
            TDerived::Process(&state.GetValueBuilder(), items, [&](TBlockItem out) {
                auto datum = builderImpl->Build(out); res->value = datum.array();
            });
        } else {
            auto& builder = state.GetArrayBuilder();
            auto* builderImpl = CastToArrayBuilderImpl<TArrayBuilderImpl>(builder);

            size_t maxBlockLength = builder.MaxLength();
            Y_ENSURE(maxBlockLength > 0);
            TVector<std::shared_ptr<arrow20::ArrayData>> outputArrays;
            // Initialize all scalar arguments before the main "process" loop.
            for (size_t k = 0; k < Argc; k++) {
                if (needUpdate[k]) {
                    continue;
                }
                auto& reader = state.GetReader(k);

                args[k] = reader.GetScalarItem(*batch[k].scalar);
            }
            for (int64_t i = 0; i < alength;) {
                for (size_t j = 0; j < maxBlockLength && i < alength; ++j, ++i) {
                    // Update array arguments and call the Process routine.
                    for (size_t k = 0; k < Argc; k++) {
                        if (!needUpdate[k]) {
                            continue;
                        }
                        auto& reader = state.GetReader(k);

                        args[k] = reader.GetItem(*batch[k].array.ToArrayData(), i);
                    }
                    TDerived::Process(&state.GetValueBuilder(), items, [&](TBlockItem out) {
                        builderImpl->Add(out);
                    });
                }
                auto outputDatum = builderImpl->Build(false);
                ForEachArrayData(outputDatum, [&](const auto& arr) { outputArrays.push_back(arr); });
            }

            auto resultDatum = MakeArray(outputArrays);
            res->value = resultDatum.array();
        }

        return arrow20::Status::OK();
    }
};

template <typename TInput, typename TOutput, TOutput (*Core)(TInput)>
arrow20::Status UnaryPreallocatedExecImpl(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
    Y_UNUSED(ctx);
    auto& inArray = batch.values[0].array;
    auto& outArrayData = *std::get<std::shared_ptr<arrow20::ArrayData>>(res->value);
    const TInput* inValues = inArray.GetValues<TInput>(1);
    TOutput* outValues = outArrayData.GetMutableValues<TOutput>(1);
    auto length = inArray.length;
    for (int64_t i = 0; i < length; ++i) {
        outValues[i] = Core(inValues[i]);
    }

    return arrow20::Status::OK();
}

template <typename TReader, typename TOutput, TOutput (*Core)(TBlockItem)>
arrow20::Status UnaryPreallocatedReaderExecImpl(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
    Y_UNUSED(ctx);
    static_assert(std::is_base_of_v<IBlockReader, TReader>);
    TReader reader;

    auto& inArray = batch.values[0].array;
    auto& outArrayData = *std::get<std::shared_ptr<arrow20::ArrayData>>(res->value);
    TOutput* outValues = outArrayData.GetMutableValues<TOutput>(1);
    auto length = inArray.length;
    for (int64_t i = 0; i < length; ++i) {
        auto item = reader.GetItem(*inArray.ToArrayData(), i);
        outValues[i] = Core(item);
    }

    return arrow20::Status::OK();
}

template <typename TInput, typename TOutput, std::pair<TOutput, bool> Core(TInput)>
struct TUnaryUnsafeFixedSizeFilterKernel {
    static arrow20::Status Do(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
        static_assert(std::is_arithmetic<TInput>::value);

        Y_UNUSED(ctx);
        const auto& inArray = batch.values.front().array;
        const auto* inValues = inArray.GetValues<TInput>(1);

        const auto length = inArray.length;

        auto& outArrayData = *std::get<std::shared_ptr<arrow20::ArrayData>>(res->value);
        auto* outValues = outArrayData.GetMutableValues<TOutput>(1);

        TTypedBufferBuilder<uint8_t> nullBuilder(GetYqlMemoryPool());
        nullBuilder.Reserve(length);

        bool isAllNull = inArray.GetNullCount() == length;
        if (!isAllNull) {
            for (i64 i = 0; i < length; ++i) {
                auto [output, isValid] = Core(inValues[i]);
                outValues[i] = output;
                nullBuilder.UnsafeAppend(isValid);
            }
        } else {
            nullBuilder.UnsafeAppend(length, 0);
        }
        auto validMask = nullBuilder.Finish();
        validMask = MakeDenseBitmap(validMask->data(), length, GetYqlMemoryPool());

        auto inMask = inArray.GetBuffer(0);
        if (inMask && inMask->data()) {
            outArrayData.buffers[0] = AllocateBitmapWithReserve(length, GetYqlMemoryPool());
            arrow20::internal::BitmapAnd(validMask->data(), 0, inMask->data(), inArray.offset, outArrayData.length, outArrayData.offset, outArrayData.buffers[0]->mutable_data());
        } else {
            outArrayData.buffers[0] = std::move(validMask);
        }

        return arrow20::Status::OK();
    }
};

template <typename TInput, typename TOutput, TOutput (*Core)(TInput)>
class TUnaryOverOptionalImpl: public TBoxedValue {
public:
    TUnboxedValue Run(const IValueBuilder* valueBuilder, const TUnboxedValuePod* args) const final {
        Y_UNUSED(valueBuilder);
        if (!args[0]) {
            return {};
        }

        return TUnboxedValuePod(Core(args[0].Get<TInput>()));
    }
};

} // namespace NUdf
} // namespace NYql

#define BEGIN_ARROW_UDF_IMPL(udfNameBlocks, signatureFunc, optArgc, isStrict)                           \
    class udfNameBlocks {                                                                               \
    public:                                                                                             \
        typedef bool TTypeAwareMarker;                                                                  \
        static const ::NYql::NUdf::TStringRef& Name() {                                                 \
            static auto name = ::NYql::NUdf::TStringRef::Of(#udfNameBlocks).Substring(1, 256);          \
            return name;                                                                                \
        }                                                                                               \
        static bool IsStrict() {                                                                        \
            return isStrict;                                                                            \
        }                                                                                               \
        static ::NYql::NUdf::TType* GetSignatureType(::NYql::NUdf::IFunctionTypeInfoBuilder& builder) { \
            return builder.SimpleSignatureType<signatureFunc>(optArgc);                                 \
        }                                                                                               \
        static bool DeclareSignature(                                                                   \
            const ::NYql::NUdf::TStringRef& name,                                                       \
            ::NYql::NUdf::TType* userType,                                                              \
            ::NYql::NUdf::IFunctionTypeInfoBuilder& builder,                                            \
            bool typesOnly);                                                                            \
    };

#define BEGIN_SIMPLE_ARROW_UDF(udfName, signatureFunc)                  \
    BEGIN_ARROW_UDF_IMPL(udfName##_BlocksImpl, signatureFunc, 0, false) \
    UDF_IMPL(udfName, builder.SimpleSignature<signatureFunc>().SupportsBlocks();, ;, ;, "", "", udfName##_BlocksImpl)

#define BEGIN_SIMPLE_STRICT_ARROW_UDF(udfName, signatureFunc)          \
    BEGIN_ARROW_UDF_IMPL(udfName##_BlocksImpl, signatureFunc, 0, true) \
    UDF_IMPL(udfName, builder.SimpleSignature<signatureFunc>().SupportsBlocks().IsStrict();, ;, ;, "", "", udfName##_BlocksImpl)

#define BEGIN_SIMPLE_STRICT_ARROW_UDF_OPTIONS(udfName, signatureFunc, options) \
    BEGIN_ARROW_UDF_IMPL(udfName##_BlocksImpl, signatureFunc, 0, true)         \
    UDF_IMPL(udfName, builder.SimpleSignature<signatureFunc>().SupportsBlocks().IsStrict(); options;, ;, ;, "", "", udfName##_BlocksImpl)

#define BEGIN_SIMPLE_ARROW_UDF_WITH_OPTIONAL_ARGS(udfName, signatureFunc, optArgc) \
    BEGIN_ARROW_UDF_IMPL(udfName##_BlocksImpl, signatureFunc, optArgc, false)      \
    UDF_IMPL(udfName, builder.SimpleSignature<signatureFunc>().SupportsBlocks().OptionalArgs(optArgc);, ;, ;, "", "", udfName##_BlocksImpl)

#define BEGIN_SIMPLE_STRICT_ARROW_UDF_WITH_OPTIONAL_ARGS(udfName, signatureFunc, optArgc) \
    BEGIN_ARROW_UDF_IMPL(udfName##_BlocksImpl, signatureFunc, optArgc, true)              \
    UDF_IMPL(udfName, builder.SimpleSignature<signatureFunc>().SupportsBlocks().IsStrict().OptionalArgs(optArgc);, ;, ;, "", "", udfName##_BlocksImpl)

#define END_ARROW_UDF(udfNameBlocks, exec)                                                                       \
    inline bool udfNameBlocks::DeclareSignature(                                                                 \
        const ::NYql::NUdf::TStringRef& name,                                                                    \
        ::NYql::NUdf::TType* userType,                                                                           \
        ::NYql::NUdf::IFunctionTypeInfoBuilder& builder,                                                         \
        bool typesOnly) {                                                                                        \
        if (Name() == name) {                                                                                    \
            if (IsStrict()) {                                                                                    \
                builder.IsStrict();                                                                              \
            }                                                                                                    \
            PrepareSimpleArrowUdf(builder, GetSignatureType(builder), userType, exec, typesOnly, TString(name)); \
            return true;                                                                                         \
        }                                                                                                        \
        return false;                                                                                            \
    }

#define END_ARROW_UDF_WITH_NULL_HANDLING(udfNameBlocks, exec, nullHandling)                                                    \
    inline bool udfNameBlocks::DeclareSignature(                                                                               \
        const ::NYql::NUdf::TStringRef& name,                                                                                  \
        ::NYql::NUdf::TType* userType,                                                                                         \
        ::NYql::NUdf::IFunctionTypeInfoBuilder& builder,                                                                       \
        bool typesOnly) {                                                                                                      \
        if (Name() == name) {                                                                                                  \
            PrepareSimpleArrowUdf(builder, GetSignatureType(builder), userType, exec, typesOnly, TString(name), nullHandling); \
            return true;                                                                                                       \
        }                                                                                                                      \
        return false;                                                                                                          \
    }

#define END_SIMPLE_ARROW_UDF(udfName, exec) \
    END_ARROW_UDF(udfName##_BlocksImpl, exec)

#define END_SIMPLE_ARROW_UDF_WITH_NULL_HANDLING(udfName, exec, nullHandling) \
    END_ARROW_UDF_WITH_NULL_HANDLING(udfName##_BlocksImpl, exec, nullHandling)
