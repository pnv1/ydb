#include "mkql_block_container.h"

#include <yql/essentials/minikql/computation/mkql_block_impl.h>

#include <yql/essentials/minikql/arrow/arrow_defs.h>
#include <yql/essentials/minikql/arrow/arrow_util.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_node_builder.h>

#include <arrow/util/bitmap_ops.h>

namespace NKikimr {
namespace NMiniKQL {

namespace {

class TBlockAsContainerExec {
public:
    TBlockAsContainerExec(const TVector<TType*>& argTypes, const std::shared_ptr<arrow20::DataType>& returnArrowType)
        : ArgTypes(argTypes)
        , ReturnArrowType(returnArrowType)
    {
    }

    arrow20::Status Exec(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) const {
        bool allScalars = true;
        size_t length = 0;
        for (int i = 0; i < batch.num_values(); ++i) {
            const auto& x = batch[i];
            if (x.kind() != arrow20::Datum::SCALAR) {
                allScalars = false;
                length = x.array.length;
                break;
            }
        }

        if (allScalars) {
            // return scalar too
            std::vector<std::shared_ptr<arrow20::Scalar>> arrowValue;
            for (int i = 0; i < batch.num_values(); ++i) {
                arrowValue.emplace_back(batch[i].scalar);
            }

            res->value = arrow20::Datum(std::make_shared<arrow20::StructScalar>(arrowValue, ReturnArrowType));
            return arrow20::Status::OK();
        }

        auto newArrayData = arrow20::ArrayData::Make(ReturnArrowType, length, {nullptr}, 0, 0);
        MKQL_ENSURE(ArgTypes.size() == static_cast<size_t>(batch.num_values()), "Mismatch batch columns");
        for (int i = 0; i < batch.num_values(); ++i) {
            const auto& arg = batch[i];
            if (arg.kind() == arrow20::Datum::SCALAR) {
                // expand scalar to array
                auto expandedArray = MakeArrayFromScalar(*arg.scalar, length, AS_TYPE(TBlockType, ArgTypes[i])->GetItemType(), *ctx->memory_pool());
                newArrayData->child_data.push_back(expandedArray.array());
            } else {
                newArrayData->child_data.push_back(arg.array.ToArray()->data());
            }
        }

        res->value = arrow20::Datum(newArrayData);
        return arrow20::Status::OK();
    }

private:
    const TVector<TType*> ArgTypes;
    const std::shared_ptr<arrow20::DataType> ReturnArrowType;
};

arrow20::Status BlockAsContainerKernelExec(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
    auto* exec = reinterpret_cast<TBlockAsContainerExec*>(ctx->kernel()->data.get());
    return exec->Exec(ctx, batch, res);
}

std::shared_ptr<arrow20::compute::ScalarKernel> MakeBlockAsContainerKernel(const TVector<TType*>& argTypes, TType* resultType) {
    std::shared_ptr<arrow20::DataType> returnArrowType;
    MKQL_ENSURE(ConvertArrowType(AS_TYPE(TBlockType, resultType)->GetItemType(), returnArrowType), "Unsupported arrow type");
    auto exec = std::make_shared<TBlockAsContainerExec>(argTypes, returnArrowType);
    auto kernel = std::make_shared<arrow20::compute::ScalarKernel>(
        arrow20::compute::KernelSignature::Make(ConvertToInputTypes(argTypes), ConvertToOutputType(resultType)),
        BlockAsContainerKernelExec);

    kernel->null_handling = arrow20::compute::NullHandling::COMPUTED_NO_PREALLOCATE;
    kernel->data = std::reinterpret_pointer_cast<arrow20::compute::KernelState>(exec);
    return kernel;
}

} // namespace

IComputationNode* WrapBlockAsContainer(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    TComputationNodePtrVector argsNodes;
    TVector<TType*> argsTypes;
    for (ui32 i = 0; i < callable.GetInputsCount(); ++i) {
        argsNodes.push_back(LocateNode(ctx.NodeLocator, callable, i));
        argsTypes.push_back(callable.GetInput(i).GetStaticType());
    }

    auto kernel = MakeBlockAsContainerKernel(argsTypes, callable.GetType()->GetReturnType());
    return new TBlockFuncNode(ctx.Mutables, ToDatumValidateMode(ctx.ValidateMode), callable.GetType()->GetName(), std::move(argsNodes), argsTypes, callable.GetType()->GetReturnType(), *kernel, kernel);
}

} // namespace NMiniKQL
} // namespace NKikimr
