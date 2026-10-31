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

ConstraintValidationResult ConstraintValidator::validateFkConstraintForDelete(
    const std::string &targetTableName,
    const std::vector<std::string> &tableNames,
    const StorageEngine &storageEngine,
    const std::vector<LocatedRow> &delRows

)
{
    using constraint_detail::Key;
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
                for (const auto &locatedRow : delRows)
                {
                    const auto &row = locatedRow.row;
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
    const HeaderPage &schema, const std::vector<Row> &rows,
    const std::set<RowId> &excludedRows)
{
    return validateConstraint<const std::vector<Row> &>(schema, rows, excludedRows);
}

StorageConstraintLookup::StorageConstraintLookup(StorageEngine &storageEngine)
    : storageEngine_(storageEngine)
{
}

// bool StorageConstraintLookup::keyExists(
//     const std::string &tableName, std::span<const KeyPart> key)
// {
//     auto table = storageEngine_.openTable(tableName);
//     auto cursor = table.scan();
//     while (auto row = cursor.next())
//     {
//         if (matchesKeys(*row, key))
//             return true;
//     }
//     return false;
// }

bool StorageConstraintLookup::keyExists(
    const std::string &tableName,
    std::span<const KeyPart> key,
    const std::set<RowId> &excludedRows)
{
    auto table = storageEngine_.openTable(tableName);
    auto cursor = table.scan();
    while (auto locatedRow = cursor.nextLocated())
    {

        if (excludedRows.contains(locatedRow->id))
            continue;
        if (matchesKeys(locatedRow->row, key))
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
