#pragma once

#include "db_expression_evaluator.h"
#include "db_storage_engine.h"

#include <algorithm>
#include <format>
#include <type_traits>
#include <variant>

// Validation revisits the rows for each constraint, so its ranges must be
// multipass. Definitions live here to support caller-created transform views.
namespace constraint_detail
{
    using Key = std::vector<KeyPart>;

    inline bool rowMatchesKey(const Row &row, std::span<const KeyPart> key)
    {
        return std::ranges::all_of(key, [&](const KeyPart &part)
                                   { return row.values.at(part.columnId) == part.value; });
    }

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

}

template <std::ranges::forward_range Rows>
ConstraintValidationResult ConstraintValidator::validateFkConstraintForUpdate(
    const std::string &targetTableName,
    const std::vector<std::string> &tableNames,
    StorageEngine &storageEngine,
    Rows &&newRows,
    const std::set<RowId> &excludedRows)
{
    using constraint_detail::Key;
    using constraint_detail::rowMatchesKey;
    for (const auto &tableName : tableNames)
    {
        const auto schema = storageEngine.getTableHeader(tableName);
        for (const auto &constraint : schema.constraints)
        {
            const auto *fk =
                std::get_if<BoundForeignKeyConstraintExpr>(&constraint);

            if (!fk || fk->referencedTableName != targetTableName)
                continue;

            const auto childHasParent = [&](const Row &childRow)
            {
                Key parentKey;
                parentKey.reserve(fk->localColumnIds.size());
                for (std::size_t i = 0; i < fk->localColumnIds.size(); ++i)
                {
                    const Value &value = childRow.values.at(fk->localColumnIds.at(i));
                    // A NULL in any component means this child references no parent.
                    if (std::holds_alternative<std::monostate>(value))
                        return true;
                    parentKey.push_back({fk->referencedColumnIds.at(i), value});
                }

                // The final parent table consists of replacements plus stored
                // rows that this UPDATE does not replace.
                const bool matchesReplacement = std::ranges::any_of(
                    newRows, [&](const Row &parentRow)
                    { return rowMatchesKey(parentRow, parentKey); });
                return matchesReplacement ||
                       lookup_.keyExists(targetTableName, parentKey, excludedRows);
            };

            const auto failure = [&]() -> ConstraintValidationResult
            {
                return {
                    .valid = false,
                    .constraintName = fk->constraintName,
                    .message = std::format(
                        "FOREIGN KEY constraint '{}' in table '{}' has no matching row "
                        "in table '{}' after UPDATE",
                        fk->constraintName, tableName, targetTableName)};
            };

            auto childTable = storageEngine.openTable(tableName);
            auto cursor = childTable.scan();
            while (auto child = cursor.nextLocated())
            {
                // Row IDs are table-local. Only a self-reference uses the
                // same exclusions on the child side of the relationship.
                if (tableName == targetTableName && excludedRows.contains(child->id))
                    continue;
                if (!childHasParent(child->row))
                    return failure();
            }

            if (tableName == targetTableName)
            {
                // Replaced self-referencing children must be checked using
                // their new FK values, not their old stored values.
                for (const Row &childRow : newRows)
                {
                    if (!childHasParent(childRow))
                        return failure();
                }
            }
        }
    }
    return {.valid = true, .constraintName = std::nullopt, .message = std::nullopt};
}

template <std::ranges::forward_range Rows>
ConstraintValidationResult ConstraintValidator::validateUpdateConstraint(
    const HeaderPage &schema,
    Rows &&rows,
    const std::set<RowId> &excludedRows,
    const std::vector<std::string> &tableNames,
    StorageEngine &storageEngine)
{
    auto result = validateConstraint(schema, rows, excludedRows);
    if (!result.valid)
    {
        return result;
    }
    return validateFkConstraintForUpdate(
        schema.tableName, tableNames, storageEngine, rows, excludedRows);
}

template <std::ranges::forward_range Rows>
ConstraintValidationResult ConstraintValidator::validateConstraint(
    const HeaderPage &schema, Rows &&rows, const std::set<RowId> &excludedRows)
{
    using constraint_detail::Key;
    using constraint_detail::KeyLess;
    using constraint_detail::rowMatchesKey;

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
                            lookup_.keyExists(schema.tableName, key, excludedRows))
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
                        const bool updatingSelfReference =
                            constraintValue.referencedTableName == schema.tableName &&
                            !excludedRows.empty();
                        const bool parentExists = updatingSelfReference
                                                      ? std::ranges::any_of(rows, [&](const Row &parentRow)
                                                                            { return rowMatchesKey(parentRow, referencedKey); }) ||
                                                            lookup_.keyExists(schema.tableName, referencedKey, excludedRows)
                                                      : lookup_.keyExists(constraintValue.referencedTableName, referencedKey);
                        if (!parentExists)
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

