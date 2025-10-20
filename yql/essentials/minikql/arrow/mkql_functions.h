#pragma once

#include <yql/essentials/minikql/mkql_node.h>
#include <arrow/datum.h>

namespace NKikimr::NMiniKQL {

class IBuiltinFunctionRegistry;

bool FindArrowFunction(TStringBuf name, const TArrayRef<TType*>& inputTypes, TType* outputType, const IBuiltinFunctionRegistry& registry);
bool ConvertInputArrowType(TType* blockType, arrow20::TypeHolder& descr);
bool HasArrowCast(TType* from, TType* to);
} // namespace NKikimr::NMiniKQL
