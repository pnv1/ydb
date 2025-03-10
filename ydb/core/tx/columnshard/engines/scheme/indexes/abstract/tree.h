#pragma once
#include "common.h"
#include "like.h"

#include <ydb/core/formats/arrow/program/abstract.h>
#include <ydb/core/tx/program/program.h>

#include <ydb/library/accessor/accessor.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/scalar.h>
#include <library/cpp/json/writer/json_value.h>
#include <yql/essentials/core/arrow_kernels/request/request.h>

namespace NKikimr::NOlap::NIndexes::NRequest {

class IRequestNode {
protected:
    TNodeId NodeId;
    std::vector<std::shared_ptr<IRequestNode>> Children;
    IRequestNode* Parent = nullptr;
    virtual bool DoCollapse() = 0;

    virtual NJson::TJsonValue DoSerializeToJson() const = 0;
    virtual std::shared_ptr<IRequestNode> DoCopy() const = 0;

public:
    template <class T>
    T* FindFirst() const {
        for (auto&& c : Children) {
            if (auto* result = c->As<T>()) {
                return result;
            }
        }
        return nullptr;
    }

    std::shared_ptr<IRequestNode> Copy() const;

    const std::vector<std::shared_ptr<IRequestNode>>& GetChildren() const {
        return Children;
    }

    IRequestNode(const TNodeId& nodeId)
        : NodeId(nodeId) {
    }

    virtual ~IRequestNode() = default;

    template <class T>
    bool Is() const {
        return dynamic_cast<const T*>(this);
    }

    template <class T>
    T* As() {
        return dynamic_cast<T*>(this);
    }

    void RemoveChildren(const TNodeId nodeId);

    const TNodeId& GetNodeId() const {
        return NodeId;
    }

    virtual bool Collapse() {
        for (auto&& i : Children) {
            if (i->Collapse()) {
                return true;
            }
        }
        if (DoCollapse()) {
            return true;
        }
        return false;
    }

    void Attach(const std::vector<std::shared_ptr<IRequestNode>>& children) {
        auto copy = children;
        for (auto&& c : copy) {
            Attach(c);
        }
    }

    void Attach(const std::shared_ptr<IRequestNode>& children);

    void Exchange(const TNodeId& nodeId, const std::shared_ptr<IRequestNode>& children);

    NJson::TJsonValue SerializeToJson() const;
};

class TConstantNode: public IRequestNode {
private:
    using TBase = IRequestNode;
    YDB_READONLY_DEF(std::shared_ptr<arrow::Scalar>, Constant);

protected:
    virtual NJson::TJsonValue DoSerializeToJson() const override {
        NJson::TJsonValue result = NJson::JSON_MAP;
        result.InsertValue("type", "const");
        result.InsertValue("const", Constant->ToString());
        return result;
    }
    virtual bool DoCollapse() override {
        return false;
    }
    virtual std::shared_ptr<IRequestNode> DoCopy() const override {
        return std::make_shared<TConstantNode>(GetNodeId().GetColumnId(), Constant);
    }

public:
    TConstantNode(const ui32 columnId, const std::shared_ptr<arrow::Scalar>& constant)
        : TBase(TNodeId::Constant(columnId))
        , Constant(constant) {
    }
};

class TRootNode: public IRequestNode {
private:
    using TBase = IRequestNode;

protected:
    virtual bool DoCollapse() override {
        return false;
    }
    virtual NJson::TJsonValue DoSerializeToJson() const override {
        NJson::TJsonValue result = NJson::JSON_MAP;
        result.InsertValue("type", "ROOT");
        return result;
    }

    virtual std::shared_ptr<IRequestNode> DoCopy() const override {
        return nullptr;
    }

public:
    TRootNode()
        : TBase(TNodeId::RootNodeId()) {
    }
};

class TOriginalColumn: public IRequestNode {
private:
    using TBase = IRequestNode;

protected:
    virtual bool DoCollapse() override {
        return false;
    }
    virtual NJson::TJsonValue DoSerializeToJson() const override {
        NJson::TJsonValue result = NJson::JSON_MAP;
        result.InsertValue("type", "column");
        return result;
    }
    virtual std::shared_ptr<IRequestNode> DoCopy() const override {
        return std::make_shared<TOriginalColumn>(GetNodeId().GetColumnId());
    }

public:
    TOriginalColumn(const ui32 columnId, const TString& subColumnName = "")
        : TBase(TNodeId::Original(columnId, subColumnName)) {
    }
};

class TPackAnd: public IRequestNode {
private:
    using TBase = IRequestNode;
    THashMap<TOriginalDataAddress, std::shared_ptr<arrow::Scalar>> Equals;
    THashMap<TOriginalDataAddress, TLikeDescription> Likes;
    bool IsEmptyFlag = false;

protected:
    virtual bool DoCollapse() override {
        return false;
    }
    virtual NJson::TJsonValue DoSerializeToJson() const override;
    virtual std::shared_ptr<IRequestNode> DoCopy() const override {
        return std::make_shared<TPackAnd>(*this);
    }

public:
    TPackAnd(const TPackAnd&) = default;

    TPackAnd(const TOriginalDataAddress& originalDataAddress, const std::shared_ptr<arrow::Scalar>& value)
        : TBase(TNodeId::Aggregation()) {
        AddEqual(originalDataAddress, value);
    }

    TPackAnd(const TOriginalDataAddress& originalDataAddress, const TLikePart& part)
        : TBase(TNodeId::Aggregation()) {
        AddLike(originalDataAddress, TLikeDescription(part));
    }

    const THashMap<TOriginalDataAddress, std::shared_ptr<arrow::Scalar>>& GetEquals() const {
        return Equals;
    }

    const THashMap<TOriginalDataAddress, TLikeDescription>& GetLikes() const {
        return Likes;
    }

    bool IsEmpty() const {
        return IsEmptyFlag;
    }
    void AddEqual(const TOriginalDataAddress& originalDataAddress, const std::shared_ptr<arrow::Scalar>& value);
    void AddLike(const TOriginalDataAddress& originalDataAddress, const TLikeDescription& value) {
        auto it = Likes.find(originalDataAddress);
        if (it == Likes.end()) {
            Likes.emplace(originalDataAddress, value);
        } else {
            it->second.Merge(value);
        }
    }
    void Merge(const TPackAnd& add) {
        for (auto&& i : add.Equals) {
            AddEqual(i.first, i.second);
        }
        for (auto&& i : add.Likes) {
            AddLike(i.first, i.second);
        }
    }
};

class TOperationNode: public IRequestNode {
private:
    using TBase = IRequestNode;
    NYql::TKernelRequestBuilder::EBinaryOp Operation;

protected:
    virtual NJson::TJsonValue DoSerializeToJson() const override {
        NJson::TJsonValue result = NJson::JSON_MAP;
        result.InsertValue("type", "operation");
        result.InsertValue("operation", ::ToString(Operation));
        return result;
    }

    virtual bool DoCollapse() override;
    virtual std::shared_ptr<IRequestNode> DoCopy() const override {
        std::vector<std::shared_ptr<IRequestNode>> children;
        return std::make_shared<TOperationNode>(GetNodeId().GetColumnId(), Operation, children);
    }

public:
    NYql::TKernelRequestBuilder::EBinaryOp GetOperation() const {
        return Operation;
    }

    TOperationNode(
        const ui32 columnId, const NYql::TKernelRequestBuilder::EBinaryOp& operation, const std::vector<std::shared_ptr<IRequestNode>>& args)
        : TBase(TNodeId::Operation(columnId))
        , Operation(operation) {
        for (auto&& i : args) {
            Attach(i);
        }
    }
};

class TKernelNode: public IRequestNode {
private:
    using TBase = IRequestNode;
    const TString KernelName;

protected:
    virtual NJson::TJsonValue DoSerializeToJson() const override {
        NJson::TJsonValue result = NJson::JSON_MAP;
        result.InsertValue("type", "operation");
        result.InsertValue("kernel_name", KernelName);
        return result;
    }

    virtual bool DoCollapse() override;
    virtual std::shared_ptr<IRequestNode> DoCopy() const override {
        std::vector<std::shared_ptr<IRequestNode>> children;
        return std::make_shared<TKernelNode>(GetNodeId().GetColumnId(), KernelName, children);
    }

public:
    const TString GetKernelName() const {
        return KernelName;
    }

    TKernelNode(const ui32 columnId, const TString kernelName, const std::vector<std::shared_ptr<IRequestNode>>& args)
        : TBase(TNodeId::Operation(columnId))
        , KernelName(kernelName) {
        for (auto&& i : args) {
            Attach(i);
        }
    }
};

class TNormalForm {
private:
    std::map<ui32, std::shared_ptr<IRequestNode>> Nodes;
    std::map<ui32, std::shared_ptr<IRequestNode>> NodesGlobal;

public:
    TNormalForm() = default;

    bool Add(const NArrow::NSSA::IResourceProcessor& processor, const TProgramContainer& program);

    std::shared_ptr<TRootNode> GetRootNode();
};

}   // namespace NKikimr::NOlap::NIndexes::NRequest
