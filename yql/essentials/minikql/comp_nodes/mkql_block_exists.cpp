#include "mkql_exists.h"
#include <yql/essentials/minikql/arrow/arrow_util.h>
#include <yql/essentials/minikql/computation/mkql_block_impl.h>
#include <yql/essentials/minikql/mkql_node_cast.h>

namespace NKikimr {
namespace NMiniKQL {

namespace {

class TBlockExistsExec {
public:
    arrow20::Status Exec(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) const {
        const auto& input = batch[0];
        if (input.is_scalar()) {
            auto scalarResult = arrow20::MakeArrayFromScalar(arrow20::UInt8Scalar(static_cast<ui8>(input.scalar->is_valid)), 1, ctx->memory_pool());
            if (!scalarResult.ok()) {
                return scalarResult.status();
            }
            res->value = (*scalarResult)->data();
            return arrow20::Status::OK();
        }
        MKQL_ENSURE(input.is_array(), "Expected array");
        const auto& arr = input.array;

        auto nullCount = arr.GetNullCount();
        if (nullCount == arr.length) {
            res->value = MakeFalseArray(ctx->memory_pool(), arr.length).array();
        } else if (nullCount == 0) {
            res->value = MakeTrueArray(ctx->memory_pool(), arr.length).array();
        } else {
            res->value = MakeBitmapArray(ctx->memory_pool(), arr.length, arr.offset,
                                   arr.buffers[0].data).array();
        }

        return arrow20::Status::OK();
    }
};

arrow20::Status BlockExistsKernelExec(arrow20::compute::KernelContext* ctx, const arrow20::compute::ExecSpan& batch, arrow20::compute::ExecResult* res) {
    auto* exec = reinterpret_cast<TBlockExistsExec*>(ctx->kernel()->data.get());
    return exec->Exec(ctx, batch, res);
}

std::shared_ptr<arrow20::compute::ScalarKernel> MakeBlockExistsKernel(const TVector<TType*>& argTypes, TType* resultType) {
    std::shared_ptr<arrow20::DataType> returnArrowType;
    MKQL_ENSURE(ConvertArrowType(AS_TYPE(TBlockType, resultType)->GetItemType(), returnArrowType), "Unsupported arrow type");
    // Ensure the result Arrow type (i.e. boolean) is Arrow UInt8Type.
    Y_DEBUG_ABORT_UNLESS(returnArrowType == arrow20::uint8());
    auto exec = std::make_shared<TBlockExistsExec>();
    auto kernel = std::make_shared<arrow20::compute::ScalarKernel>(
        arrow20::compute::KernelSignature::Make(ConvertToInputTypes(argTypes), ConvertToOutputType(resultType)),
        BlockExistsKernelExec);
    kernel->data = std::reinterpret_pointer_cast<arrow20::compute::KernelState>(exec);
    kernel->null_handling = arrow20::compute::NullHandling::OUTPUT_NOT_NULL;
    return kernel;
}

} // namespace

IComputationNode* WrapBlockExists(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 1, "Expected 1 arg");
    auto compute = LocateNode(ctx.NodeLocator, callable, 0);
    TComputationNodePtrVector argsNodes = {compute};
    TVector<TType*> argsTypes = {callable.GetInput(0).GetStaticType()};
    auto kernel = MakeBlockExistsKernel(argsTypes, callable.GetType()->GetReturnType());
    return new TBlockFuncNode(ctx.Mutables, ToDatumValidateMode(ctx.ValidateMode), "Exists", std::move(argsNodes), argsTypes, callable.GetType()->GetReturnType(), *kernel, kernel);
}

} // namespace NMiniKQL
} // namespace NKikimr
