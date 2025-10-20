#include "mkql_block_getelem.h"

#include <yql/essentials/minikql/computation/mkql_block_impl.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_node_builder.h>
#include <yql/essentials/minikql/mkql_type_helper.h>
#include <yql/essentials/public/udf/udf_type_printer.h>

namespace NKikimr {
namespace NMiniKQL {

namespace {

inline bool IsOptionalOrNull(const TType* type) {
    return type->IsOptional() || type->IsNull() || type->IsPg();
}

enum class EOptionalityHandlerStrategy {
    // Just return child as is.
    ReturnChildAsIs,
    // Return child and add optionality to it.
    AddOptionalToChild,
    // Return child but set mask to (tuple & child) intersection.
    IntersectOptionals,
    // Return null.
    ReturnNull,
};

// The strategy is based on tuple and its child optionality.
// Tuple<X> -> return child as is (ReturnChildAsIs).
// Tuple<X?> -> return child as is (ReturnChildAsIs).
// Tuple<X>? -> return child and add extra optional level (AddOptionalToChild).
// Tuple<X?>? -> return child as is BUT set mask to (tuple & child) intersection (IntersectOptionals).
// Tuple<Null>? -> return Null (ReturnNull).
EOptionalityHandlerStrategy GetStrategyBasedOnTupleType(TType* tupleType, TType* elementType) {
    if (!tupleType->IsOptional()) {
        return EOptionalityHandlerStrategy::ReturnChildAsIs;
    } else if (elementType->IsNull()) {
        return EOptionalityHandlerStrategy::ReturnNull;
    } else if (IsOptionalOrNull(elementType)) {
        return EOptionalityHandlerStrategy::IntersectOptionals;
    } else {
        return EOptionalityHandlerStrategy::AddOptionalToChild;
    }
}

std::shared_ptr<arrow20::Buffer> CreateBitmapIntersection(arrow20::compute::KernelContext* ctx,
                                                        std::shared_ptr<arrow20::ArrayData> tuple,
                                                        size_t index,
                                                        bool preserveOffset) {
    auto child = tuple->child_data[index];
    auto resultBitmapOffset = preserveOffset ? child->offset : 0;
    if (!tuple->buffers[0]) {
        return child->buffers[0];
    } else if (!child->buffers[0]) {
        auto buffer = AllocateBitmapWithReserve(child->length + resultBitmapOffset, ctx->memory_pool());
        arrow20::internal::CopyBitmap(tuple->GetValues<uint8_t>(0, 0), tuple->offset,
                                    child->length, buffer->mutable_data(), resultBitmapOffset);
        return buffer;
    } else {
        auto buffer = AllocateBitmapWithReserve(child->length + resultBitmapOffset, ctx->memory_pool());
        arrow20::internal::BitmapAnd(child->GetValues<uint8_t>(0, 0), child->offset,
                                   tuple->GetValues<uint8_t>(0, 0), tuple->offset,
                                   child->length,
                                   resultBitmapOffset, buffer->mutable_data());
        return buffer;
    }
}

class TAddOptionalLevelHelper {
public:
    TAddOptionalLevelHelper(TType* returnType, std::shared_ptr<arrow20::DataType> returnArrowType)
        : IsReturnExternalOptional_(NeedWrapWithExternalOptional(returnType))
        , ReturnArrowType_(returnArrowType)
    {
    }

    std::shared_ptr<arrow20::ArrayData> AddOptionalToChild(std::shared_ptr<arrow20::ArrayData> tuple, size_t index, arrow20::compute::KernelContext* ctx) const {
        if (IsReturnExternalOptional_) {
            return arrow20::ArrayData::Make(ReturnArrowType_, tuple->length, {tuple->buffers[0]}, {tuple->child_data[index]}, arrow20::kUnknownNullCount, tuple->offset);
        } else {
            auto child = tuple->child_data[index];
            auto bitmask = MakeDenseBitmapCopyIfOffsetDiffers(tuple->buffers[0], child->length, tuple->offset, child->offset, ctx->memory_pool());
            return SetOptional(*child, bitmask, child->offset);
        }
    }

    std::shared_ptr<arrow20::Scalar> AddOptionalToChildOfValidTuple(const arrow20::StructScalar& tuple, size_t index) const {
        if (IsReturnExternalOptional_) {
            return std::make_shared<arrow20::StructScalar>(std::vector<std::shared_ptr<arrow20::Scalar>>{tuple.value[index]}, ReturnArrowType_);
        } else {
            return tuple.value[index];
        }
    }

    std::shared_ptr<arrow20::Scalar> IntersectOptionalsOfValidTuple(const arrow20::StructScalar& tuple, size_t index) const {
        auto child = tuple.value[index];
        return child;
    }

    std::shared_ptr<arrow20::ArrayData> IntersectOptionals(std::shared_ptr<arrow20::ArrayData> tuple, size_t index, arrow20::compute::KernelContext* ctx) const {
        auto child = tuple->child_data[index];
        if (IsReturnExternalOptional_) {
            auto intersection = CreateBitmapIntersection(ctx, tuple, index, /*preserveOffset=*/false);
            return SetOptional(*child, intersection, 0);
        } else {
            auto intersection = CreateBitmapIntersection(ctx, tuple, index, /*preserveOffset=*/true);
            return SetOptional(*child, intersection, tuple->child_data[index]->offset);
        }
    }

    std::shared_ptr<arrow20::ArrayData> SetOptional(const arrow20::ArrayData& input, std::shared_ptr<arrow20::Buffer> bitmask, size_t offset) const {
        auto result = input.Copy();
        result->buffers[0] = bitmask;
        result->offset = offset;
        result->SetNullCount(arrow20::kUnknownNullCount);
        return result;
    }

    bool NeedAddExtraOptionalLevel() const {
        return IsReturnExternalOptional_;
    }

private:
    const bool IsReturnExternalOptional_;
    const std::shared_ptr<arrow20::DataType> ReturnArrowType_;
};

class TBlockGetElementExec {
public:
    TBlockGetElementExec(const std::shared_ptr<arrow20::DataType>& returnArrowType, ui32 index, EOptionalityHandlerStrategy resultStrategy, TAddOptionalLevelHelper addOptionalHelper)
        : ReturnArrowType(returnArrowType)
        , Index(index)
        , ResultStrategy(resultStrategy)
        , AddOptionalHelper(std::move(addOptionalHelper))
    {
    }

    arrow20::Status ExecArray(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) const {
        const auto& inputArg = batch[0];
        MKQL_ENSURE(inputArg.kind() != arrow20::Datum::SCALAR, "Array expected.");

        const auto& tuple = inputArg.array.ToArray()->data();
        auto child = tuple->child_data[Index];
        switch (ResultStrategy) {
            case EOptionalityHandlerStrategy::ReturnChildAsIs: {
                res->value = arrow20::Datum(child);
                return arrow20::Status::OK();
            }
            case EOptionalityHandlerStrategy::AddOptionalToChild: {
                res->value = arrow20::Datum(AddOptionalHelper.AddOptionalToChild(tuple, Index, ctx));
                return arrow20::Status::OK();
            }
            case EOptionalityHandlerStrategy::IntersectOptionals: {
                res->value = arrow20::Datum(AddOptionalHelper.IntersectOptionals(tuple, Index, ctx));
                return arrow20::Status::OK();
            }
            case EOptionalityHandlerStrategy::ReturnNull:
                res->value = NYql::NUdf::MakeSingularArray(/*isNull=*/true, tuple->length);
                return arrow20::Status::OK();
        }

        return arrow20::Status::OK();
    }

    arrow20::Status ExecScalar(const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) const {
        const auto& inputArg = batch[0];
        MKQL_ENSURE(inputArg.kind() == arrow20::Datum::SCALAR, "Scalar expected.");

        if (!inputArg.scalar->is_valid) {
            res->value = arrow20::Datum(arrow20::MakeNullScalar(ReturnArrowType));
            return arrow20::Status::OK();
        }
        const auto& tuple = arrow20::internal::checked_cast<const arrow20::StructScalar&>(*inputArg.scalar);

        switch (ResultStrategy) {
            case EOptionalityHandlerStrategy::ReturnChildAsIs: {
                auto datum = arrow20::Datum(tuple.value[Index]);
                res->value = datum.array.ToArrayData();
                return arrow20::Status::OK();
            }
            case EOptionalityHandlerStrategy::AddOptionalToChild: {
                auto datum = arrow20::Datum(AddOptionalHelper.AddOptionalToChildOfValidTuple(tuple, Index));
                res->value = datum.array.ToArrayData();
                return arrow20::Status::OK();
            }
            case EOptionalityHandlerStrategy::IntersectOptionals: {
                auto datum = arrow20::Datum(AddOptionalHelper.IntersectOptionalsOfValidTuple(tuple, Index));
                res->value = datum.array.ToArrayData();
                return arrow20::Status::OK();
            }
            case EOptionalityHandlerStrategy::ReturnNull: {
                auto scalar = NYql::NUdf::MakeSingularScalar(/*IsNull=*/true);
                auto arrayResult = arrow20::MakeArrayFromScalar(*scalar, 1);
                if (!arrayResult.ok()) {
                    return arrayResult.status();
                }
                res->value = (*arrayResult)->data();
                return arrow20::Status::OK();
            }

        return arrow20::Status::OK();
    }

    arrow20::Status Exec(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) const {
        if (!batch[0].is_scalar()) {
            return ExecArray(ctx, batch, res);
        } else {
            return ExecScalar(batch, res);
        }
    }

private:
    const std::shared_ptr<arrow20::DataType> ReturnArrowType;
    const ui32 Index;
    EOptionalityHandlerStrategy ResultStrategy;
    TAddOptionalLevelHelper AddOptionalHelper;
};

arrow20::Status BlockGetElementKernelExec(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
    auto* exec = reinterpret_cast<TBlockGetElementExec*>(ctx->kernel()->data.get());
    return exec->Exec(ctx, batch, res);
}

std::shared_ptr<arrow20::compute::ScalarKernel> MakeBlockGetElementKernel(const TVector<TType*>& argTypes, TType* resultType,
                                                                        ui32 index, EOptionalityHandlerStrategy resultStrategy, TAddOptionalLevelHelper addOptionalHelper) {
    std::shared_ptr<arrow20::DataType> returnArrowType;
    MKQL_ENSURE(ConvertArrowType(AS_TYPE(TBlockType, resultType)->GetItemType(), returnArrowType), "Unsupported arrow type");
    auto exec = std::make_shared<TBlockGetElementExec>(returnArrowType, index, resultStrategy, std::move(addOptionalHelper));
    auto kernel = std::make_shared<arrow20::compute::ScalarKernel>(
        arrow20::compute::KernelSignature::Make(ConvertToInputTypes(argTypes), ConvertToOutputType(resultType)),
        BlockGetElementKernelExec);

    kernel->null_handling = arrow20::compute::NullHandling::COMPUTED_NO_PREALLOCATE;
    kernel->data = std::reinterpret_pointer_cast<arrow20::compute::KernelState>(exec);
    return kernel;
}

TType* GetElementType(const TStructType* structType, ui32 index) {
    MKQL_ENSURE(index < structType->GetMembersCount(), "Bad member index");
    return structType->GetMemberType(index);
}

TType* GetElementType(const TTupleType* tupleType, ui32 index) {
    MKQL_ENSURE(index < tupleType->GetElementsCount(), "Bad tuple index");
    return tupleType->GetElementType(index);
}

template <typename ObjectType>
IComputationNode* WrapBlockGetElement(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 2, "Expected two args.");
    auto inputObject = callable.GetInput(0);
    auto blockTupleType = AS_TYPE(TBlockType, inputObject.GetStaticType());
    bool isOptional;
    auto objectType = AS_TYPE(ObjectType, UnpackOptional(blockTupleType->GetItemType(), isOptional));
    auto index = AS_VALUE(TDataLiteral, callable.GetInput(1))->AsValue().Get<ui32>();
    auto tupleElementType = GetElementType(objectType, index);
    EOptionalityHandlerStrategy strategy = GetStrategyBasedOnTupleType(blockTupleType->GetItemType(), tupleElementType);

    auto* returnType = AS_TYPE(TBlockType, callable.GetType()->GetReturnType());
    std::shared_ptr<arrow20::DataType> returnArrowType;

    MKQL_ENSURE(ConvertArrowType(returnType->GetItemType(), returnArrowType), "Unsupported arrow type");
    TAddOptionalLevelHelper addOptionalHelper(returnType->GetItemType(), returnArrowType);

    auto objectNode = LocateNode(ctx.NodeLocator, callable, 0);

    TComputationNodePtrVector argsNodes = {objectNode};
    TVector<TType*> argsTypes = {blockTupleType};
    auto kernel = MakeBlockGetElementKernel(argsTypes, returnType, index, strategy, std::move(addOptionalHelper));
    return new TBlockFuncNode(ctx.Mutables, ToDatumValidateMode(ctx.ValidateMode), callable.GetType()->GetName(), std::move(argsNodes), argsTypes, callable.GetType()->GetReturnType(), *kernel, kernel);
}

} // namespace

IComputationNode* WrapBlockMember(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    return WrapBlockGetElement<TStructType>(callable, ctx);
}

IComputationNode* WrapBlockNth(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    return WrapBlockGetElement<TTupleType>(callable, ctx);
}

} // namespace NMiniKQL
} // namespace NKikimr
