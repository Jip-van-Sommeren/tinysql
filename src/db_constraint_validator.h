#pragma once

#include "db_storage.h"

#include <span>
#include <set>
#include <ranges>

class StorageEngine;

struct KeyPart
{
    ColumnId columnId;
    Value value;
};

struct ConstraintValidationResult
{
    bool valid;
    std::optional<std::string> constraintName;
    std::optional<std::string> message;
};

class ConstraintLookup
{
public:
    virtual ~ConstraintLookup() = default;

    virtual bool keyExists(
        const std::string &tableName,
        std::span<const KeyPart> key,
        const std::set<RowId> &excludedRows = {}) = 0;
};

class ConstraintValidator final
{
public:
    explicit ConstraintValidator(ConstraintLookup &lookup)
        : lookup_(lookup)
    {
    }
    template <std::ranges::forward_range Rows>
    ConstraintValidationResult validateConstraint(
        const HeaderPage &schema, Rows &&rows, const std::set<RowId> &excludedRows = {});

    // Also accepts braced batches used by typed storage callers.
    ConstraintValidationResult validateConstraint(
        const HeaderPage &schema, const std::vector<Row> &rows,
        const std::set<RowId> &excludedRows = {});

    template <std::ranges::forward_range Rows>
    ConstraintValidationResult validateUpdateConstraint(
        const HeaderPage &schema,
        Rows &&rows,
        const std::set<RowId> &excludedRows,
        const std::vector<std::string> &tableNames,
        StorageEngine &storageEngine);

    ConstraintValidationResult validateFkConstraintForTableCreation(
        const std::vector<BoundConstraint> &constraints,
        const StorageEngine &storageEngine);

    ConstraintValidationResult validateFkConstraintForDelete(
        const std::string &targetTableName,
        const std::vector<std::string> &tableNames,
        const StorageEngine &storageEngine,
        const std::vector<LocatedRow> &delRows);

    template <std::ranges::forward_range Rows>
    ConstraintValidationResult validateFkConstraintForUpdate(
        const std::string &targetTableName,
        const std::vector<std::string> &tableNames,
        StorageEngine &storageEngine,
        Rows &&newRows,
        const std::set<RowId> &excludedRows);

private:
    ConstraintLookup &lookup_;
};

class StorageConstraintLookup final : public ConstraintLookup
{
public:
    explicit StorageConstraintLookup(StorageEngine &storageEngine);

    bool keyExists(
        const std::string &tableName,
        std::span<const KeyPart> key,
        const std::set<RowId> &excludedRows = {}) override;

private:
    StorageEngine &storageEngine_;
    bool matchesKeys(const Row &row, std::span<const KeyPart> key);
};

#include "db_constraint_validator.tpp"
