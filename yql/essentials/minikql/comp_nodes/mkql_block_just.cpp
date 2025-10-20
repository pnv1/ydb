#include "mkql_block_just.h"

#include <yql/essentials/minikql/computation/mkql_block_impl.h>
#include <yql/essentials/minikql/arrow/arrow_defs.h>
#include <yql/essentials/minikql/arrow/arrow_util.h>
#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/minikql/mkql_type_helper.h>

namespace NKikimr {
namespace NMiniKQL {

namespace {

template <bool Trivial>
class TJustBlockExec {
public:
    TJustBlockExec(const std::shared_ptr<arrow20::DataType>& returnArrowType)
        : ReturnArrowType(returnArrowType)
    {
    }

    arrow20::Status Exec(arrow20::compute::KernelContext*, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) const {
        const auto& inputArg = batch[0];
        if (Trivial) {
            if (inputArg.is_scalar()) {
                auto arrayResult = arrow20::MakeArrayFromScalar(*inputArg.scalar, 1);
                if (!arrayResult.ok()) {
                    return arrayResult.status();
                }
                res->value = (*arrayResult)->data();
            } else {
                res->value = inputArg.array.ToArray()->data();
            }
            return arrow20::Status::OK();
        }

        if (inputArg.is_scalar()) {
            std::vector<std::shared_ptr<arrow20::Scalar>> arrowValue;
            arrowValue.emplace_back(inputArg.scalar);
            auto structScalar = std::make_shared<arrow20::StructScalar>(arrowValue, ReturnArrowType);
            auto arrayResult = arrow20::MakeArrayFromScalar(*structScalar, 1);
            if (!arrayResult.ok()) {
                return arrayResult.status();
            }
            res->value = (*arrayResult)->data();
        } else {
            auto array = inputArg.array.ToArray()->data();
            auto newArrayData = arrow20::ArrayData::Make(ReturnArrowType, array->length, {nullptr}, 0, 0);
            newArrayData->child_data.push_back(array);
            res->value = newArrayData;
        }

        return arrow20::Status::OK();
    }

private:
    const std::shared_ptr<arrow20::DataType> ReturnArrowType;
};

template <bool Trivial>
arrow20::Status BlockJustKernelExec(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
    using TExec = TJustBlockExec<Trivial>;
    auto* exec = reinterpret_cast<TExec*>(ctx->kernel()->data.get());
    return exec->Exec(ctx, batch, res);
}

template <bool Trivial>
std::shared_ptr<arrow20::compute::ScalarKernel> MakeBlockJustKernel(const TVector<TType*>& argTypes, TType* resultType) {
    using TExec = TJustBlockExec<Trivial>;

    std::shared_ptr<arrow20::DataType> returnArrowType;
    MKQL_ENSURE(ConvertArrowType(AS_TYPE(TBlockType, resultType)->GetItemType(), returnArrowType), "Unsupported arrow type");
    auto exec = std::make_shared<TExec>(returnArrowType);
    auto kernel = std::make_shared<arrow20::compute::ScalarKernel>(
        arrow20::compute::KernelSignature::Make(ConvertToInputTypes(argTypes), ConvertToOutputType(resultType)),
        BlockJustKernelExec<Trivial>);

    kernel->null_handling = arrow20::compute::NullHandling::COMPUTED_NO_PREALLOCATE;
    kernel->data = std::reinterpret_pointer_cast<arrow20::compute::KernelState>(exec);
    return kernel;
}

} // namespace

IComputationNode* WrapBlockJust(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 1, "Expected 1 args");

    auto data = callable.GetInput(0);

    auto dataType = AS_TYPE(TBlockType, data.GetStaticType());

    auto dataCompute = LocateNode(ctx.NodeLocator, callable, 0);

    TComputationNodePtrVector argsNodes = {dataCompute};
    TVector<TType*> argsTypes = {dataType};

    std::shared_ptr<arrow20::compute::ScalarKernel> kernel;
    if (NeedWrapWithExternalOptional(AS_TYPE(TBlockType, callable.GetType()->GetReturnType())->GetItemType())) {
        kernel = MakeBlockJustKernel<false>(argsTypes, callable.GetType()->GetReturnType());
    } else {
        kernel = MakeBlockJustKernel<true>(argsTypes, callable.GetType()->GetReturnType());
    }

    return new TBlockFuncNode(ctx.Mutables, ToDatumValidateMode(ctx.ValidateMode), callable.GetType()->GetName(), std::move(argsNodes), argsTypes, callable.GetType()->GetReturnType(), *kernel, kernel);
}

} // namespace NMiniKQL
} // namespace NKikimr
