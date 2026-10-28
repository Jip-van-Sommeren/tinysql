#pragma once

#include "db_storage.h"

#include <span>

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
        std::span<const KeyPart> key) = 0;
};

class ConstraintValidator final
{
public:
    explicit ConstraintValidator(ConstraintLookup &lookup)
        : lookup_(lookup)
    {
    }

    ConstraintValidationResult validateConstraint(
        const HeaderPage &headerPage,
        const std::vector<Row> &rows);

    ConstraintValidationResult validateFkConstraintForTableCreation(
        const std::vector<BoundConstraint> &constraints,
        const StorageEngine &storageEngine);

    ConstraintValidationResult checkForDelete(
        const std::string &targetTableName,
        const std::vector<std::string> &tableNames,
        const StorageEngine &storageEngine,
        const std::vector<Row> &delRows

    );

private:
    ConstraintLookup &lookup_;
};

class StorageConstraintLookup final : public ConstraintLookup
{
public:
    explicit StorageConstraintLookup(StorageEngine &storageEngine);
    bool keyExists(const std::string &tableName,
                   std::span<const KeyPart> key) override;

private:
    StorageEngine &storageEngine_;
    bool matchesKeys(const Row &row, std::span<const KeyPart> key);
};
