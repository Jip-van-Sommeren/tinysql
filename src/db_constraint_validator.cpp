#include "db_constraint_validator.h"

#include "db_expression_evaluator.h"
#include "db_query_validator.h"
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

ConstraintValidationResult ConstraintValidator::validateAlterTableConstraint(
    const HeaderPage &schema, const BoundAlterTable &alter,
    const std::vector<std::string> &tableNames, StorageEngine &storageEngine)
{
    const ConstraintValidationResult success{
        .valid = true, .constraintName = std::nullopt, .message = std::nullopt};
    const auto nameOf = [](const BoundConstraint &constraint) -> const std::string &
    {
        return std::visit([](const auto &value) -> const std::string &
                          { return value.constraintName; }, constraint);
    };

    const auto validateAddition = [&](const std::vector<BoundConstraint> &added,
                                      const Column *newColumn) -> ConstraintValidationResult
    {
        auto proposed = schema;
        if (newColumn)
            proposed.columns.push_back(*newColumn);
        std::set<std::string> names;
        bool hasPrimaryKey = false;
        for (const auto &constraint : schema.constraints)
        {
            names.insert(nameOf(constraint));
            hasPrimaryKey = hasPrimaryKey ||
                            std::holds_alternative<BoundPrimaryKeyConstraintExpr>(constraint);
        }
        for (const auto &constraint : added)
        {
            const auto &name = nameOf(constraint);
            if (!names.insert(name).second)
                return {false, name, "Constraint already exists: " + name};
            if (std::holds_alternative<BoundPrimaryKeyConstraintExpr>(constraint))
            {
                if (hasPrimaryKey)
                    return {false, name, "A table can have only one PRIMARY KEY"};
                hasPrimaryKey = true;
            }
            proposed.constraints.push_back(constraint);
        }

        // Check referenced keys even for an empty table. A self-reference must
        // resolve against the proposed schema, which may contain the new key.
        for (const auto &constraint : added)
        {
            const auto *fk = std::get_if<BoundForeignKeyConstraintExpr>(&constraint);
            if (!fk)
                continue;
            const auto referenced = fk->referencedTableName == schema.tableName
                                        ? proposed
                                        : storageEngine.getTableHeader(fk->referencedTableName);
            if (!std::ranges::any_of(referenced.constraints, [&](const BoundConstraint &candidate)
                                     { return matchesReferencedKey(candidate, fk->referencedColumnIds); }))
                return {false, fk->constraintName,
                        std::format("FOREIGN KEY '{}' must reference a PRIMARY KEY or UNIQUE key in table '{}'",
                                    fk->constraintName, fk->referencedTableName)};
        }

        const BoundDefaultConstraintExpr *defaultValue = nullptr;
        if (newColumn)
        {
            for (const auto &constraint : added)
            {
                const auto *candidate = std::get_if<BoundDefaultConstraintExpr>(&constraint);
                if (candidate && candidate->columnId == newColumn->columnIndex)
                {
                    if (defaultValue)
                        return {false, candidate->constraintName,
                                "Multiple DEFAULT constraints for column: " + newColumn->name};
                    defaultValue = candidate;
                }
            }
        }

        // Validate the final row set as a batch. Exclude all original rows so
        // UNIQUE/PK checks do not mistake a stored row for its own duplicate.
        std::vector<Row> rows;
        std::set<RowId> excludedRows;
        auto table = storageEngine.openTable(schema.tableName);
        auto cursor = table.scan();
        while (auto located = cursor.nextLocated())
        {
            excludedRows.insert(located->id);
            auto &row = located->row;
            if (newColumn)
            {
                Value value{std::monostate{}};
                if (defaultValue)
                {
                    try
                    {
                        // Let the constraint validator report NOT NULL/PK
                        // violations with the actual constraint's name.
                        auto nullableColumn = *newColumn;
                        nullableColumn.nullable = true;
                        value = QueryValidator::convertForColumn(
                            evaluateValue(*defaultValue->value, Row{}), nullableColumn);
                    }
                    catch (const QueryException &error)
                    {
                        return {false, defaultValue->constraintName, error.what()};
                    }
                    catch (const QueryNumericOverflow &error)
                    {
                        return {false, defaultValue->constraintName, error.what()};
                    }
                }
                row.values.push_back(std::move(value));
            }
            rows.push_back(std::move(row));
        }

        proposed.constraints = added;
        auto result = validateConstraint(proposed, rows, excludedRows);
        if (!result.valid)
            return result;
        // Also honor nullability supplied directly in a typed Column, without
        // an accompanying named NOT NULL constraint.
        if (newColumn && !newColumn->nullable &&
            std::ranges::any_of(rows, [&](const Row &row)
                                { return std::holds_alternative<std::monostate>(row.values.at(newColumn->columnIndex)); }))
            return {false, std::nullopt,
                    "Cannot add NOT NULL column without a non-NULL value for existing rows: " + newColumn->name};
        return success;
    };

    const auto validateIncomingReferences = [&](const auto &blocks,
                                                const std::string &operation) -> ConstraintValidationResult
    {
        std::set<std::string> tables{tableNames.begin(), tableNames.end()};
        tables.insert(schema.tableName);
        for (const auto &name : tables)
        {
            const auto childSchema = name == schema.tableName ? schema : storageEngine.getTableHeader(name);
            for (const auto &constraint : childSchema.constraints)
            {
                const auto *fk = std::get_if<BoundForeignKeyConstraintExpr>(&constraint);
                if (fk && fk->referencedTableName == schema.tableName && blocks(name, *fk))
                    return {false, fk->constraintName,
                            std::format("{} is blocked by FOREIGN KEY constraint '{}' in table '{}'",
                                        operation, fk->constraintName, name)};
            }
        }
        return success;
    };

    return std::visit(
        [&](const auto &action) -> ConstraintValidationResult
        {
            using T = std::decay_t<decltype(action)>;
            if constexpr (std::is_same_v<T, BoundRenameTableAction> ||
                          std::is_same_v<T, BoundRenameColumnAction>)
            {
                // Name checks belong to binding. Applying a rename must update
                // dependent metadata; it does not invalidate any stored values.
                return success;
            }
            else if constexpr (std::is_same_v<T, BoundAddColumnAction>)
            {
                return validateAddition(action.constraints, &action.column);
            }
            else if constexpr (std::is_same_v<T, BoundAddConstraintAction>)
            {
                return validateAddition({action.constraint}, nullptr);
            }
            else if constexpr (std::is_same_v<T, BoundDropColumnAction>)
            {
                // Local constraints and indexes, including self-referencing
                // FKs, are removed with the column. External FKs need CASCADE.
                if (action.cascade)
                    return success;
                return validateIncomingReferences(
                    [&](const std::string &child, const BoundForeignKeyConstraintExpr &fk)
                    {
                        return child != schema.tableName &&
                               std::ranges::find(fk.referencedColumnIds, action.column.columnIndex) !=
                                   fk.referencedColumnIds.end();
                    },
                    "DROP COLUMN '" + action.column.name + "' without CASCADE");
            }
            else if constexpr (std::is_same_v<T, BoundDropConstraintAction>)
            {
                const auto target = std::ranges::find_if(schema.constraints, [&](const BoundConstraint &constraint)
                                                         { return nameOf(constraint) == action.constraintName; });
                if (target == schema.constraints.end())
                    return {false, action.constraintName, "Constraint does not exist: " + action.constraintName};
                return validateIncomingReferences(
                    [&](const std::string &, const BoundForeignKeyConstraintExpr &fk)
                    {
                        if (!matchesReferencedKey(*target, fk.referencedColumnIds))
                            return false;
                        // FKs identify referenced columns, not a particular key
                        // constraint. An equivalent surviving key is sufficient.
                        return !std::ranges::any_of(schema.constraints, [&](const BoundConstraint &candidate)
                                                    { return &candidate != &*target &&
                                                             matchesReferencedKey(candidate, fk.referencedColumnIds); });
                    },
                    "DROP CONSTRAINT '" + action.constraintName + "'");
            }
        },
        alter.action);
}

std::pair<ConstraintValidationResult, std::vector<std::string>> ConstraintValidator::findReferencingTables(
    const std::string &targetTableName,
    const std::vector<std::string> &tableNames,
    const StorageEngine &storageEngine,
    bool cascade) const
{
    ConstraintValidationResult validation{
        .valid = true, .constraintName = std::nullopt, .message = std::nullopt};
    std::set<std::string> referencingTables;
    for (const auto &tableName : tableNames)
    {
        const auto schema = storageEngine.getTableHeader(tableName);
        for (const auto &constraint : schema.constraints)
        {
            const auto *fk = std::get_if<BoundForeignKeyConstraintExpr>(&constraint);
            if (!fk || fk->referencedTableName != targetTableName)
                continue;

            referencingTables.insert(schema.tableName);
            if (!cascade && schema.tableName != targetTableName && validation.valid)
            {
                validation = {
                    .valid = false,
                    .constraintName = fk->constraintName,
                    .message = std::format(
                        "FOREIGN KEY constraint '{}' in table '{}' references table '{}', DROP TABLE requires CASCADE",
                        fk->constraintName, schema.tableName, targetTableName)};
            }
            break;
        }
    }
    return {std::move(validation), {referencingTables.begin(), referencingTables.end()}};
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
