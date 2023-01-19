/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/sbe_stage_builder_abt_helpers.h"

#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/rewrites/path_lower.h"
#include "mongo/db/query/sbe_stage_builder_abt_holder_impl.h"
#include "mongo/db/query/sbe_stage_builder_const_eval.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"


namespace mongo::stage_builder {

std::unique_ptr<sbe::EExpression> makeBalancedBooleanOpTreeImpl(
    sbe::EPrimBinary::Op logicOp,
    std::vector<std::unique_ptr<sbe::EExpression>>& leaves,
    size_t from,
    size_t until) {
    invariant(from < until);
    if (from + 1 == until) {
        return std::move(leaves[from]);
    } else {
        size_t mid = (from + until) / 2;
        auto lhs = makeBalancedBooleanOpTreeImpl(logicOp, leaves, from, mid);
        auto rhs = makeBalancedBooleanOpTreeImpl(logicOp, leaves, mid, until);
        return makeBinaryOp(logicOp, std::move(lhs), std::move(rhs));
    }
}

std::unique_ptr<sbe::EExpression> makeBalancedBooleanOpTree(
    sbe::EPrimBinary::Op logicOp, std::vector<std::unique_ptr<sbe::EExpression>> leaves) {
    return makeBalancedBooleanOpTreeImpl(logicOp, leaves, 0, leaves.size());
}

optimizer::ABT makeBalancedBooleanOpTreeImpl(optimizer::Operations logicOp,
                                             std::vector<optimizer::ABT>& leaves,
                                             size_t from,
                                             size_t until) {
    invariant(from < until);
    if (from + 1 == until) {
        return std::move(leaves[from]);
    } else {
        size_t mid = (from + until) / 2;
        auto lhs = makeBalancedBooleanOpTreeImpl(logicOp, leaves, from, mid);
        auto rhs = makeBalancedBooleanOpTreeImpl(logicOp, leaves, mid, until);
        return optimizer::make<optimizer::BinaryOp>(logicOp, std::move(lhs), std::move(rhs));
    }
}

optimizer::ABT makeBalancedBooleanOpTree(optimizer::Operations logicOp,
                                         std::vector<optimizer::ABT> leaves) {
    return makeBalancedBooleanOpTreeImpl(logicOp, leaves, 0, leaves.size());
}

EvalExpr makeBalancedBooleanOpTree(sbe::EPrimBinary::Op logicOp,
                                   std::vector<EvalExpr> leaves,
                                   optimizer::SlotVarMap& slotVarMap) {
    if (std::all_of(
            leaves.begin(), leaves.end(), [](auto&& e) { return e.hasABT() || e.hasSlot(); })) {
        std::vector<optimizer::ABT> abtExprs;
        abtExprs.reserve(leaves.size());
        for (auto&& e : leaves) {
            abtExprs.push_back(abt::unwrap(e.extractABT(slotVarMap)));
        }
        return abt::wrap(makeBalancedBooleanOpTree(logicOp == sbe::EPrimBinary::logicAnd
                                                       ? optimizer::Operations::And
                                                       : optimizer::Operations::Or,
                                                   std::move(abtExprs)));
    }

    std::vector<std::unique_ptr<sbe::EExpression>> exprs;
    exprs.reserve(leaves.size());
    for (auto&& e : leaves) {
        exprs.emplace_back(e.extractExpr(slotVarMap));
    }
    return EvalExpr{makeBalancedBooleanOpTree(logicOp, std::move(exprs))};
}

std::unique_ptr<sbe::EExpression> abtToExpr(optimizer::ABT& abt, optimizer::SlotVarMap& slotMap) {
    auto env = optimizer::VariableEnvironment::build(abt);

    // Do not use descriptive names here.
    auto prefixId = optimizer::PrefixId::create(false /*useDescriptiveNames*/);
    // Convert paths into ABT expressions.
    optimizer::EvalPathLowering pathLower{prefixId, env};
    pathLower.optimize(abt);

    // Run the constant folding to eliminate lambda applications as they are not directly
    // supported by the SBE VM.
    ExpressionConstEval constEval{env, nullptr /*collator*/};  // TODO Add collator support
    constEval.optimize(abt);

    // And finally convert to the SBE expression.
    optimizer::SBEExpressionLowering exprLower{env, slotMap};
    return exprLower.optimize(abt);
}

optimizer::ABT makeFillEmpty(optimizer::ABT e, bool valueIfEmpty) {
    using namespace std::literals;
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::FillEmpty, std::move(e), optimizer::Constant::boolean(valueIfEmpty));
}

optimizer::ABT makeFillEmptyFalse(optimizer::ABT e) {
    return makeFillEmpty(std::move(e), false);
}

optimizer::ABT makeFillEmptyTrue(optimizer::ABT e) {
    return makeFillEmpty(std::move(e), true);
}

optimizer::ABT makeNot(optimizer::ABT e) {
    return optimizer::make<optimizer::UnaryOp>(optimizer::Operations::Not, std::move(e));
}

optimizer::ProjectionName makeVariableName(sbe::value::SlotId slotId) {
    // Use a naming scheme that reduces that chances of clashing into a user-created variable name.
    str::stream varName;
    varName << "__s" << slotId;
    return optimizer::ProjectionName{varName};
}

optimizer::ProjectionName makeLocalVariableName(sbe::FrameId frameId, sbe::value::SlotId slotId) {
    // Use a naming scheme that reduces that chances of clashing into a user-created variable name.
    str::stream varName;
    varName << "__l" << frameId << "." << slotId;
    return optimizer::ProjectionName{varName};
}

optimizer::ABT makeVariable(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::Variable>(std::move(var));
}

optimizer::ABT generateABTNullOrMissing(optimizer::ABT var) {
    return makeFillEmptyTrue(optimizer::make<optimizer::FunctionCall>(
        "typeMatch",
        optimizer::ABTVector{std::move(var),
                             optimizer::Constant::int32(getBSONTypeMask(BSONType::jstNULL) |
                                                        getBSONTypeMask(BSONType::Undefined))}));
}

optimizer::ABT generateABTNullOrMissing(optimizer::ProjectionName var) {
    return generateABTNullOrMissing(optimizer::make<optimizer::Variable>(std::move(var)));
}

optimizer::ABT generateABTNonStringCheck(optimizer::ABT var) {
    return makeNot(optimizer::make<optimizer::FunctionCall>("isString", optimizer::ABTVector{var}));
}

optimizer::ABT generateABTNonStringCheck(optimizer::ProjectionName var) {
    return generateABTNonStringCheck(optimizer::make<optimizer::Variable>(std::move(var)));
}

optimizer::ABT generateABTNonTimestampCheck(optimizer::ProjectionName var) {
    return makeNot(optimizer::make<optimizer::FunctionCall>(
        "isTimestamp", optimizer::ABTVector{optimizer::make<optimizer::Variable>(var)}));
}

optimizer::ABT generateABTNegativeCheck(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Lt,
                                                optimizer::make<optimizer::Variable>(var),
                                                optimizer::Constant::int32(0));
}

optimizer::ABT generateABTNonPositiveCheck(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::BinaryOp>(optimizer::Operations::Lte,
                                                optimizer::make<optimizer::Variable>(var),
                                                optimizer::Constant::int32(0));
}

optimizer::ABT generateABTPositiveCheck(optimizer::ABT var) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::Gt, std::move(var), optimizer::Constant::int32(0));
}

optimizer::ABT generateABTNonNumericCheck(optimizer::ProjectionName var) {
    return makeNot(optimizer::make<optimizer::FunctionCall>(
        "isNumber", optimizer::ABTVector{optimizer::make<optimizer::Variable>(var)}));
}

optimizer::ABT generateABTLongLongMinCheck(optimizer::ProjectionName var) {
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::And,
        optimizer::make<optimizer::FunctionCall>(
            "typeMatch",
            optimizer::ABTVector{
                optimizer::make<optimizer::Variable>(var),
                optimizer::Constant::int32(getBSONTypeMask(BSONType::NumberLong))}),
        optimizer::make<optimizer::BinaryOp>(
            optimizer::Operations::Eq,
            optimizer::make<optimizer::Variable>(var),
            optimizer::Constant::int64(std::numeric_limits<int64_t>::min())));
}

optimizer::ABT generateABTNonArrayCheck(optimizer::ProjectionName var) {
    return makeNot(makeABTFunction("isArray", optimizer::make<optimizer::Variable>(var)));
}

optimizer::ABT generateABTNonObjectCheck(optimizer::ProjectionName var) {
    return makeNot(makeABTFunction("isObject", optimizer::make<optimizer::Variable>(var)));
}

optimizer::ABT generateABTNullishOrNotRepresentableInt32Check(optimizer::ProjectionName var) {
    auto numericConvert32 = optimizer::make<optimizer::FunctionCall>(
        "convert",
        optimizer::ABTVector{
            optimizer::make<optimizer::Variable>(var),
            optimizer::Constant::int32(static_cast<int32_t>(sbe::value::TypeTags::NumberInt32))});
    return optimizer::make<optimizer::BinaryOp>(
        optimizer::Operations::Or,
        generateABTNullOrMissing(var),
        makeNot(optimizer::make<optimizer::FunctionCall>(
            "exists", optimizer::ABTVector{std::move(numericConvert32)})));
}

optimizer::ABT generateABTNaNCheck(optimizer::ProjectionName var) {
    return makeABTFunction("isNaN", optimizer::make<optimizer::Variable>(var));
}

optimizer::ABT makeABTFail(ErrorCodes::Error error, StringData errorMessage) {
    return makeABTFunction(
        "fail"_sd, optimizer::Constant::int32(error), makeABTConstant(errorMessage));
}

template <>
optimizer::ABT buildABTMultiBranchConditional(optimizer::ABT defaultCase) {
    return defaultCase;
}

optimizer::ABT buildABTMultiBranchConditionalFromCaseValuePairs(
    std::vector<ABTCaseValuePair> caseValuePairs, optimizer::ABT defaultValue) {
    return std::accumulate(
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.end())),
        std::make_reverse_iterator(std::make_move_iterator(caseValuePairs.begin())),
        std::move(defaultValue),
        [](auto&& expression, auto&& caseValuePair) {
            return buildABTMultiBranchConditional(std::move(caseValuePair), std::move(expression));
        });
}

}  // namespace mongo::stage_builder