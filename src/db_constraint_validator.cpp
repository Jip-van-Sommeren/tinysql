#include "db_constraint_validator.h"

Value boundExprToValue(const BoundExpr &bExpr)
{
    if (bExpr.kind() == BoundExprKind::Literal)
    {
        const auto &literal =
            static_cast<const BoundLiteralExpr &>(bExpr);

        const Value &value = literal.value;
        return value;
    }
}
ConstraintValidationResult ConstraintValidator::validateConstraint(const HeaderPage &schema, Row &row)
{
    for (BoundConstraint constraint : schema.constraints)
    {
        auto result = std::visit(
            [&](const auto &constraintValue) -> ConstraintValidationResult
            {
                using T = std::decay_t<decltype(constraintValue)>;
                if constexpr (std::is_same_v<T, BoundNotNullConstraintExpr>)
                {
                    const auto columnId = constraintValue.columnId;
                    if (std::holds_alternative<std::monostate>(row.values[columnId]))
                    {

                        const auto &columnName = schema.columns.at(columnId).name;

                        auto message = std::format(
                            "Column '{}.{}' cannot be NULL: constraint '{}'",
                            schema.tableName,
                            columnName,
                            constraintValue.constraintName);
                        return ConstraintValidationResult{.valid = false, .constraintName = constraintValue.constraintName, .message = message};
                    }
                }
                else if constexpr (std::is_same_v<T, BoundPrimaryKeyConstraintExpr>)
                {
                    std::vector<KeyPart> key;
                    key.reserve(constraintValue.columnIds.size());
                    for (auto columnId : constraintValue.columnIds)
                    {

                        if (std::holds_alternative<std::monostate>(row.values[columnId]))
                        {
                            return {
                                .valid = false,
                                .constraintName = constraintValue.constraintName,
                                .message = "Primary Key cannot be NULL"};
                        }

                        key.push_back({columnId, row.values.at(columnId)});
                    }
                    if (lookup_.keyExists(schema.tableName, key))
                    {
                        return {
                            .valid = false,
                            .constraintName = constraintValue.constraintName,
                            .message = "Primary Key already exists"};
                    }
                }
                else if constexpr (std::is_same_v<T, BoundForeignKeyConstraintExpr>)
                {

                    const auto &local = constraintValue.localColumnIds;
                    const auto &referenced = constraintValue.referencedColumnIds;

                    std::vector<KeyPart> referencedKey;
                    referencedKey.reserve(referenced.size());

                    for (std::size_t i = 0; i < local.size(); ++i)
                    {
                        const Value &value = row.values.at(local[i]);

                        if (std::holds_alternative<std::monostate>(value))
                        {
                            // This constraint passes; the outer loop checks the others.
                            return {
                                .valid = true,
                                .constraintName = std::nullopt,
                                .message = ""};
                        }

                        referencedKey.push_back({referenced.at(i), value});
                    }

                    if (!lookup_.keyExists(
                            constraintValue.referencedTableName,
                            referencedKey))
                    {
                        return {
                            .valid = false,
                            .constraintName = constraintValue.constraintName,
                            .message = std::format(
                                "Foreign key '{}' has no matching row in table '{}'",
                                constraintValue.constraintName,
                                constraintValue.referencedTableName)};
                    }
                }
                else if constexpr (std::is_same_v<T, BoundUniqueConstraintExpr>)
                {
                    std::vector<KeyPart> key;
                    key.reserve(constraintValue.columnIds.size());
                    for (auto columnId : constraintValue.columnIds)
                    {

                        key.push_back({columnId, row.values.at(columnId)});
                    }
                    if (lookup_.keyExists(schema.tableName, key))
                    {
                        return {
                            .valid = false,
                            .constraintName = constraintValue.constraintName,
                            .message = "Unique Key already exists"};
                    }
                }

                else if constexpr (std::is_same_v<T, BoundCheckConstraintExpr>)
                {
                    const Value evaluated =
                        evaluateValue(*constraintValue.condition, row);

                    const bool passes =
                        std::holds_alternative<std::monostate>(evaluated) ||
                        std::get<bool>(evaluated);

                    if (!passes)
                    {
                        return {
                            .valid = false,
                            .constraintName = constraintValue.constraintName,
                            .message = std::format(
                                "CHECK constraint '{}' failed on table '{}'",
                                constraintValue.constraintName,
                                schema.tableName)};
                    }
                }
            },
            constraint

        );
        if (!result.valid)
        {
            return result; // Returns from validateConstraint().
        }
    }
    return {
        .valid = true,
        .constraintName = std::nullopt,
        .message = ""};
}

StorageConstraintLookup::StorageConstraintLookup(StorageEngine &storageEngine) : storageEngine_(storageEngine)
{
}

bool StorageConstraintLookup::keyExists(std::string tableName,
                                        std::span<KeyPart> key)
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

bool matchesKeys(Row &row, std::span<KeyPart> key)
{
    for (const auto &[columnId, value] : key)
    {
        if (row.values[columnId] != value)
            return false;
    }
    return true;
}