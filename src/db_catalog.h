#pragma once
#include "db_storage.h"

class Catalog
{
public:
    virtual bool tableExists(const std::string &tableName) const = 0;
    virtual HeaderPage getTableHeader(const std::string &tableName) const = 0;
    virtual ~Catalog() = default;
};
