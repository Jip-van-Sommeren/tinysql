#pragma once

#include <compare>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#ifdef _MSC_VER
#include "msvc_stdfloat.hpp"
#else
#include <stdfloat>
#endif

#ifdef __STDCPP_FLOAT32_T__

static_assert(sizeof(std::float32_t) == 4);

#endif

using ColumnId = std::uint32_t;

enum class FunctionId : std::uint8_t
{
    Count,
    Sum,
    Avg,
    Min,
    Max,
    Abs,
    Round,
    Lower,
    Upper
};

enum class FunctionCategory : std::uint8_t
{
    Aggregate,
    Scalar
};

struct DecimalLiteral
{
    std::string text;
};

struct DecimalValue
{
    std::int64_t coefficient;
    std::uint32_t scale;
};

bool operator==(const DecimalValue &left, const DecimalValue &right);
std::strong_ordering operator<=>(
    const DecimalValue &left,
    const DecimalValue &right);
std::ostream &operator<<(std::ostream &stream, const DecimalValue &value);

using Value = std::variant<
    std::monostate,
    std::int32_t,
    std::int64_t,
    std::float32_t,
    std::float64_t,
    DecimalValue,
    std::string,
    bool>;
using NumberValue =
    std::variant<std::int32_t, std::int64_t, std::float32_t, std::float64_t, DecimalLiteral>;

enum class DataType : std::uint8_t
{
    Null = 0,
    Int = 1,
    Text = 2,
    Double = 3,
    Float = 4,
    Decimal = 5,
    BigInt = 6,
    Boolean = 7
};

enum class BinaryOperator : std::uint8_t
{
    Add,
    Subtract,
    Multiply,
    Divide,
    And,
    Or,
    Eq,
    Ne,
    Gt,
    Ge,
    Lt,
    Le
};

enum class UnaryOperator : std::uint8_t
{
    Not,
    Negate,
    Positive
};

enum class ConstraintType : std::uint8_t
{
    PrimaryKey,
    ForeignKey,
    Unique,
    NotNull,
    Null,
    Default,
    Check
};

enum class BoundExprKind : std::uint8_t
{
    Literal,
    ColumnReference,
    FunctionCall,
    Binary,
    Unary,
    IsNull
};



struct BoundExpr
{
    explicit BoundExpr(BoundExprKind kind, DataType type)
        : kind_(kind), type_(type)
    {
    }

    virtual ~BoundExpr() = default;
    virtual std::unique_ptr<BoundExpr> clone() const = 0;

    BoundExprKind kind() const noexcept
    {
        return kind_;
    }

    DataType type() const noexcept
    {
        return type_;
    }

private:
    BoundExprKind kind_;
    DataType type_;
};




struct BoundFunctionCall final : BoundExpr
{
    BoundFunctionCall(
        FunctionId id,
        FunctionCategory category,
        std::vector<std::unique_ptr<BoundExpr>> arguments,
        DataType type)
        : BoundExpr(BoundExprKind::FunctionCall, type),
          id(id),
          category(category),
          arguments(std::move(arguments))
    {
    }

    std::unique_ptr<BoundExpr> clone() const override
    {
        std::vector<std::unique_ptr<BoundExpr>> clonedArguments;
        for (const auto &arg : arguments)
        {
            clonedArguments.push_back(arg->clone());
        }
        auto result = std::make_unique<BoundFunctionCall>(
            id,
            category,
            std::move(clonedArguments),
            type());
        result->starArgument = starArgument;
        return result;
    }

    FunctionId id;
    FunctionCategory category;
    std::vector<std::unique_ptr<BoundExpr>> arguments;
    bool starArgument = false; // Indicates if the function call has a star argument (e.g., COUNT(*))
};  

struct BoundBinaryExpr final : BoundExpr
{
    BoundBinaryExpr(
        BinaryOperator op,
        std::unique_ptr<BoundExpr> left,
        std::unique_ptr<BoundExpr> right,
        DataType resultType)
        : BoundExpr(BoundExprKind::Binary, resultType),
          op(op),
          left(std::move(left)),
          right(std::move(right))
    {
    }

    std::unique_ptr<BoundExpr> clone() const override
    {
        return std::make_unique<BoundBinaryExpr>(
            op,
            left->clone(),
            right->clone(),
            type());
    }

    BinaryOperator op;
    std::unique_ptr<BoundExpr> left;
    std::unique_ptr<BoundExpr> right;
};

struct BoundUnaryExpr final : BoundExpr
{
    BoundUnaryExpr(
        UnaryOperator op,
        std::unique_ptr<BoundExpr> operand,
        DataType type)
        : BoundExpr(BoundExprKind::Unary, type),
          op(op),
          operand(std::move(operand))
    {
    }

    std::unique_ptr<BoundExpr> clone() const override
    {
        return std::make_unique<BoundUnaryExpr>(
            op,
            operand->clone(),
            type());
    }

    UnaryOperator op;
    std::unique_ptr<BoundExpr> operand;
};

struct BoundColumnExpr final : BoundExpr
{
    BoundColumnExpr(std::uint32_t columnIndex, DataType type)
        : BoundExpr(BoundExprKind::ColumnReference, type),
          columnIndex(columnIndex)
    {
    }

    std::unique_ptr<BoundExpr> clone() const override
    {
        return std::make_unique<BoundColumnExpr>(columnIndex, type());
    }

    std::uint32_t columnIndex;
};

struct BoundLiteralExpr final : BoundExpr
{
    BoundLiteralExpr(Value value, DataType type)
        : BoundExpr(BoundExprKind::Literal, type),
          value(std::move(value))
    {
    }

    std::unique_ptr<BoundExpr> clone() const override
    {
        return std::make_unique<BoundLiteralExpr>(value, type());
    }

    Value value;
};

struct BoundIsNullExpr final : BoundExpr
{
    BoundIsNullExpr(std::unique_ptr<BoundExpr> operand, bool negated)
        : BoundExpr(BoundExprKind::IsNull, DataType::Boolean),
          operand(std::move(operand)),
          negated(negated)
    {
    }

    std::unique_ptr<BoundExpr> clone() const override
    {
        return std::make_unique<BoundIsNullExpr>(operand->clone(), negated);
    }

    std::unique_ptr<BoundExpr> operand;
    bool negated;
};

struct BoundConstraintExpr
{
    BoundConstraintExpr(
        ConstraintType type,
        std::string constraintName)
        : constraintType(type),
          constraintName(std::move(constraintName))
    {
    }

    virtual ~BoundConstraintExpr() = default;

    ConstraintType constraintType;
    std::string constraintName;
};

struct BoundPrimaryKeyConstraintExpr final : BoundConstraintExpr
{
    BoundPrimaryKeyConstraintExpr(
        std::string constraintName,
        std::vector<ColumnId> columnIds)
        : BoundConstraintExpr(ConstraintType::PrimaryKey, std::move(constraintName)),
          columnIds(std::move(columnIds))
    {
    }

    std::vector<ColumnId> columnIds;
};

struct BoundUniqueConstraintExpr final : BoundConstraintExpr
{
    BoundUniqueConstraintExpr(
        std::string constraintName,
        std::vector<ColumnId> columnIds)
        : BoundConstraintExpr(ConstraintType::Unique, std::move(constraintName)),
          columnIds(std::move(columnIds))
    {
    }

    std::vector<ColumnId> columnIds;
};

struct BoundCheckConstraintExpr final : BoundConstraintExpr
{
    BoundCheckConstraintExpr(
        std::string constraintName,
        std::unique_ptr<BoundExpr> condition)
        : BoundConstraintExpr(ConstraintType::Check, std::move(constraintName)),
          condition(std::move(condition))
    {
    }

    BoundCheckConstraintExpr(const BoundCheckConstraintExpr &other)
        : BoundConstraintExpr(other),
          condition(other.condition ? other.condition->clone() : nullptr)
    {
    }

    BoundCheckConstraintExpr &operator=(const BoundCheckConstraintExpr &other)
    {
        if (this != &other)
        {
            constraintType = other.constraintType;
            constraintName = other.constraintName;
            condition = other.condition ? other.condition->clone() : nullptr;
        }
        return *this;
    }

    BoundCheckConstraintExpr(BoundCheckConstraintExpr &&) noexcept = default;
    BoundCheckConstraintExpr &operator=(BoundCheckConstraintExpr &&) noexcept = default;

    std::unique_ptr<BoundExpr> condition;
};

struct BoundDefaultConstraintExpr final : BoundConstraintExpr
{
    BoundDefaultConstraintExpr(
        std::string constraintName,
        std::unique_ptr<BoundExpr> value,
        ColumnId columnId)
        : BoundConstraintExpr(ConstraintType::Default, std::move(constraintName)),
          value(std::move(value)),
          columnId(columnId)
    {
    }

    BoundDefaultConstraintExpr(const BoundDefaultConstraintExpr &other)
        : BoundConstraintExpr(other),
          value(other.value ? other.value->clone() : nullptr),
          columnId(other.columnId)
    {
    }

    BoundDefaultConstraintExpr &operator=(const BoundDefaultConstraintExpr &other)
    {
        if (this != &other)
        {
            constraintType = other.constraintType;
            constraintName = other.constraintName;
            value = other.value ? other.value->clone() : nullptr;
            columnId = other.columnId;
        }
        return *this;
    }

    BoundDefaultConstraintExpr(BoundDefaultConstraintExpr &&) noexcept = default;
    BoundDefaultConstraintExpr &operator=(BoundDefaultConstraintExpr &&) noexcept = default;

    std::unique_ptr<BoundExpr> value;
    ColumnId columnId;
};

struct BoundNotNullConstraintExpr final : BoundConstraintExpr
{
    BoundNotNullConstraintExpr(std::string constraintName, ColumnId columnId)
        : BoundConstraintExpr(ConstraintType::NotNull, std::move(constraintName)),
          columnId(columnId)
    {
    }

    ColumnId columnId;
};

struct BoundNullConstraintExpr final : BoundConstraintExpr
{
    BoundNullConstraintExpr(std::string constraintName, ColumnId columnId)
        : BoundConstraintExpr(ConstraintType::Null, std::move(constraintName)),
          columnId(columnId)
    {
    }

    ColumnId columnId;
};

struct BoundForeignKeyConstraintExpr final : BoundConstraintExpr
{
    BoundForeignKeyConstraintExpr(
        std::string constraintName,
        std::vector<ColumnId> localColumnIds,
        std::string referencedTableName,
        std::vector<ColumnId> referencedColumnIds)
        : BoundConstraintExpr(ConstraintType::ForeignKey, std::move(constraintName)),
          localColumnIds(std::move(localColumnIds)),
          referencedTableName(std::move(referencedTableName)),
          referencedColumnIds(std::move(referencedColumnIds))
    {
    }

    std::vector<ColumnId> localColumnIds;
    std::string referencedTableName;
    std::vector<ColumnId> referencedColumnIds;
};

using Constraint = std::variant<
    BoundPrimaryKeyConstraintExpr,
    BoundForeignKeyConstraintExpr,
    BoundUniqueConstraintExpr,
    BoundNotNullConstraintExpr,
    BoundNullConstraintExpr,
    BoundDefaultConstraintExpr,
    BoundCheckConstraintExpr>;
