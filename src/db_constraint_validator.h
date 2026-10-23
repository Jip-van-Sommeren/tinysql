#pragma once

#include "db_storage.h"
#include "db_storage_engine.h"

struct KeyPart
{
    ColumnId columnId;
    Value value;
};

struct ConstraintValidationResult
{
    bool valid;
    std::optional<std::string> constraintName;
    std::string message;
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
        Row &row);

private:
    ConstraintLookup &lookup_;
};

class ConstraintLookup
{
public:
    virtual ~ConstraintLookup() = default;

    virtual bool keyExists(
        std::string tableName,

        std::span<KeyPart> key) = 0;
};

class StorageConstraintLookup final : public ConstraintLookup
{
public:
    explicit StorageConstraintLookup(StorageEngine &storageEngine);
    bool keyExists(std::string tableName,
                   std::span<KeyPart> key) override;

private:
    StorageEngine &storageEngine_;
};