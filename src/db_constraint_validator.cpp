#include "db_constraint_validator.h"

#include "db_expression_evaluator.h"
#include "db_row_validator.h"
#include "db_storage_engine.h"

#include <algorithm>
#include <format>
#include <set>
#include <type_traits>
#include <variant>
#include <stdexcept>

namespace
{
    using Key = std::vector<KeyPart>;

    struct KeyLess
    {
        bool operator()(const Key &left, const Key &right) const
        {
            return std::lexicographical_compare(
                left.begin(), left.end(), right.begin(), right.end(),
                [](const KeyPart &a, const KeyPart &b)
                {
                    if (a.columnId != b.columnId)
                        return a.columnId < b.columnId;
                    return a.value < b.value;
                });
        }
    };

    bool matchesReferencedKey(
        const BoundConstraint &constraint,
        const std::vector<ColumnId> &referencedColumns)
    {
        if (const auto *pk =
                std::get_if<BoundPrimaryKeyConstraintExpr>(&constraint))
        {
            return std::ranges::is_permutation(
                pk->columnIds, referencedColumns);
        }

        if (const auto *unique =
                std::get_if<BoundUniqueConstraintExpr>(&constraint))
        {
            return std::ranges::is_permutation(
                unique->columnIds, referencedColumns);
        }

        return false;
    }

}

ConstraintValidationResult ConstraintValidator::checkForDelete(
    const std::string &targetTableName,
    const std::vector<std::string> &tableNames,
    const StorageEngine &storageEngine,
    const std::vector<Row> &delRows

)
{
    for (auto tableName : tableNames)
    {
        const auto schema = storageEngine.getTableHeader(tableName);
        for (const auto &constraint : schema.constraints)
        {
            const auto *fk =
                std::get_if<BoundForeignKeyConstraintExpr>(&constraint);

            if (!fk)
                continue;

            if (fk->referencedTableName == targetTableName)
            {
                for (const auto &row : delRows)
                {
                    const auto &local = fk->localColumnIds;
                    const auto &referenced = fk->referencedColumnIds;
                    Key childKey;
                    bool hasNull = false;
                    childKey.reserve(referenced.size());

                    for (std::size_t i = 0; i < referenced.size(); ++i)
                    {
                        const Value &value = row.values.at(referenced.at(i));

                        if (std::holds_alternative<std::monostate>(value))
                        {
                            hasNull = true;
                            break;
                        }

                        childKey.push_back({local.at(i), value});
                    }

                    if (hasNull)
                        continue;

                    if (lookup_.keyExists(schema.tableName, childKey))
                    {
                        return {
                            .valid = false,
                            .constraintName = fk->constraintName,
                            .message = std::format(
                                "FOREIGN KEY constraint '{}' has matching row in table '{}', DELETE not allowed",
                                fk->constraintName,
                                schema.tableName)};
                    }
                }
            }
        }
    }
    return {.valid = true, .constraintName = std::nullopt, .message = std::nullopt};
}

ConstraintValidationResult ConstraintValidator::validateFkConstraintForTableCreation(
    const std::vector<BoundConstraint> &constraints,
    const StorageEngine &storageEngine)
{
    for (const auto &constraint : constraints)
    {
        const auto *fk =
            std::get_if<BoundForeignKeyConstraintExpr>(&constraint);

        if (!fk)
            continue;

        const auto schema =
            storageEngine.getTableHeader(fk->referencedTableName);

        const bool hasMatchingKey = std::ranges::any_of(
            schema.constraints,
            [&](const BoundConstraint &candidate)
            {
                return matchesReferencedKey(
                    candidate, fk->referencedColumnIds);
            });

        if (!hasMatchingKey)
        {
            return {.valid = false,
                    .constraintName = fk->constraintName,
                    .message = std::format("FOREIGN KEY '{}' must reference a PRIMARY KEY "
                                           "or UNIQUE key in table '{}'",
                                           fk->constraintName, fk->referencedTableName)};
        }
    }
    return {.valid = true, .constraintName = std::nullopt, .message = std::nullopt};
}

ConstraintValidationResult ConstraintValidator::validateConstraint(
    const HeaderPage &schema, const std::vector<Row> &rows)
{

    for (const BoundConstraint &constraint : schema.constraints)
    {
        // Each constraint owns its batch keys, including redundant constraints
        // on the same columns. Composite keys are compared as whole tuples.
        std::set<Key, KeyLess> seenKeys;
        for (const Row &row : rows)
        {
            const auto result = std::visit(
                [&](const auto &constraintValue) -> ConstraintValidationResult
                {
                    using T = std::decay_t<decltype(constraintValue)>;
                    if constexpr (std::is_same_v<T, BoundNotNullConstraintExpr>)
                    {
                        const auto columnId = constraintValue.columnId;
                        if (std::holds_alternative<std::monostate>(row.values.at(columnId)))
                        {
                            return {
                                .valid = false,
                                .constraintName = constraintValue.constraintName,
                                .message = std::format(
                                    "Column '{}.{}' cannot be NULL: constraint '{}'",
                                    schema.tableName, schema.columns.at(columnId).name,
                                    constraintValue.constraintName)};
                        }
                    }
                    else if constexpr (std::is_same_v<T, BoundPrimaryKeyConstraintExpr> ||
                                       std::is_same_v<T, BoundUniqueConstraintExpr>)
                    {
                        Key key;
                        key.reserve(constraintValue.columnIds.size());
                        for (const auto columnId : constraintValue.columnIds)
                        {
                            const Value &value = row.values.at(columnId);
                            if (std::holds_alternative<std::monostate>(value))
                            {
                                if constexpr (std::is_same_v<T, BoundPrimaryKeyConstraintExpr>)
                                {
                                    return {
                                        .valid = false,
                                        .constraintName = constraintValue.constraintName,
                                        .message = "PRIMARY KEY cannot be NULL"};
                                }
                                else
                                {
                                    // A NULL in any UNIQUE key component exempts
                                    // this row from both stored and batch checks.
                                    return {.valid = true, .constraintName = std::nullopt, .message = ""};
                                }
                            }
                            key.push_back({columnId, value});
                        }

                        // NaN is unequal to itself under the Value equality used
                        // by storage lookup. Such keys cannot be duplicates and
                        // must not enter a set ordered by floating-point '<'.
                        const bool comparable = std::ranges::all_of(
                            key, [](const KeyPart &part)
                            { return part.value == part.value; });
                        if ((comparable && !seenKeys.insert(key).second) ||
                            lookup_.keyExists(schema.tableName, key))
                        {
                            if constexpr (std::is_same_v<T, BoundPrimaryKeyConstraintExpr>)
                            {
                                return {
                                    .valid = false,
                                    .constraintName = constraintValue.constraintName,
                                    .message = "PRIMARY KEY must be unique"};
                            }
                            else
                            {
                                return {
                                    .valid = false,
                                    .constraintName = constraintValue.constraintName,
                                    .message = std::format(
                                        "UNIQUE constraint '{}' failed on table '{}'",
                                        constraintValue.constraintName, schema.tableName)};
                            }
                        }
                    }
                    else if constexpr (std::is_same_v<T, BoundForeignKeyConstraintExpr>)
                    {
                        const auto &local = constraintValue.localColumnIds;
                        const auto &referenced = constraintValue.referencedColumnIds;
                        Key referencedKey;
                        referencedKey.reserve(referenced.size());
                        for (std::size_t i = 0; i < local.size(); ++i)
                        {
                            const Value &value = row.values.at(local[i]);
                            if (std::holds_alternative<std::monostate>(value))
                            {
                                // Only this constraint passes; later constraints
                                // must still be checked for this row.
                                return {.valid = true, .constraintName = std::nullopt, .message = ""};
                            }
                            referencedKey.push_back({referenced.at(i), value});
                        }
                        if (!lookup_.keyExists(constraintValue.referencedTableName, referencedKey))
                        {
                            return {
                                .valid = false,
                                .constraintName = constraintValue.constraintName,
                                .message = std::format(
                                    "FOREIGN KEY constraint '{}' has no matching row in table '{}'",
                                    constraintValue.constraintName,
                                    constraintValue.referencedTableName)};
                        }
                    }
                    else if constexpr (std::is_same_v<T, BoundCheckConstraintExpr>)
                    {
                        const Value evaluated = evaluateValue(*constraintValue.condition, row);
                        if (!std::holds_alternative<std::monostate>(evaluated) &&
                            !std::get<bool>(evaluated))
                        {
                            return {
                                .valid = false,
                                .constraintName = constraintValue.constraintName,
                                .message = std::format(
                                    "CHECK constraint '{}' failed on table '{}'",
                                    constraintValue.constraintName, schema.tableName)};
                        }
                    }
                    // DEFAULT has already been applied during binding, and
                    // explicit NULL declarations impose no further restriction.
                    return {.valid = true, .constraintName = std::nullopt, .message = ""};
                },
                constraint);
            if (!result.valid)
                return result;
        }
    }
    return {.valid = true, .constraintName = std::nullopt, .message = std::nullopt};
}

StorageConstraintLookup::StorageConstraintLookup(StorageEngine &storageEngine)
    : storageEngine_(storageEngine)
{
}

bool StorageConstraintLookup::keyExists(
    const std::string &tableName, std::span<const KeyPart> key)
{
    auto table = storageEngine_.openTable(tableName);
    auto cursor = table.scan();
    while (auto row = cursor.next())
    {
        if (matchesKeys(*row, key))
            return true;
    }
    return false;
}

bool StorageConstraintLookup::matchesKeys(const Row &row, std::span<const KeyPart> key)
{
    for (const auto &[columnId, value] : key)
    {
        if (row.values.at(columnId) != value)
            return false;
    }
    return true;
}
