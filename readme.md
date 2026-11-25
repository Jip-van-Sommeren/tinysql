# db

A relational database engine written in C++23, with its own SQL parser, expression evaluator, page storage, B+ tree indexes, and statement recovery. The project builds a reusable library, `db_core`, and a small demonstration executable, `db`.

The implementation targets Linux and is under active development. It supports a subset of SQL; the current scope and limitations are listed below.

## Build

Requirements:

- Linux with the `openat2` system call available.
- A C++23 compiler and standard library supporting `<expected>`, `<format>`, `<stdfloat>`, and chrono parsing/time zones. The current development build uses GCC 14.2 with libstdc++.
- CMake 3.20 or later and a build tool such as Make or Ninja.
- `pkg-config`, libuuid development files, and zlib development files.
- System time-zone data for parsing local date/time values.

From the repository root:

```sh
cmake -S . -B build \
    -DCMAKE_CXX_COMPILER=g++-14 \
    -DCMAKE_BUILD_TYPE=Debug \
    -DBUILD_TESTING=ON
cmake --build build --target db -j 2
```

The build enables strict compiler warnings and treats warnings as errors. To build only the library, use `--target db_core`.

## Run the demo

```sh
./build/db
```

The executable runs a fixed set of example queries and prints their results. It is not an interactive SQL shell and does not accept a database path argument.

**Each run deletes and recreates `/tmp/query_test_db`.** Use the C++ API below for a database you want to keep. The demo queries and output formatting are in [src/main.cpp](src/main.cpp).

## Use the C++ API

[Database](src/db_database.h) is the public entry point. `create()` requires a new database directory whose parent already exists; `open()` opens an existing database and runs pending recovery. Submit one SQL statement per call.

Save this as `example.cpp`:

```cpp
#include "db_database.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <variant>

int main(int argc, char **argv) try
{
    if (argc != 2)
    {
        std::cerr << "Usage: ./example <new-database-directory>\n";
        return 1;
    }

    auto database = Database::create(argv[1], "example");
    database.executeSql(
        "CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL);");
    database.executeSql(
        "INSERT INTO users VALUES (1, 'Ada'), (2, 'Grace');");

    // An expected query error can be reported without ending the application.
    auto rejected = database.tryExecuteSql(
        "INSERT INTO users VALUES (1, 'Duplicate');");
    if (!rejected)
        std::cerr << "Query rejected: " << rejected.error().message << '\n';

    const auto result = database.executeSql("SELECT id, name FROM users;");
    for (const auto &row : result.rows)
    {
        std::cout << std::get<std::int32_t>(row.values[0]) << '\t'
                  << std::get<std::string>(row.values[1]) << '\n';
    }
}
catch (const std::exception &error)
{
    std::cerr << "Database error: " << error.what() << '\n';
    return 1;
}
```

Compile and run it against the built library:

```sh
g++-14 -std=c++23 -I src example.cpp build/libdb_core.a -luuid -lz -o example
./example ./example-db
```

To reopen the database in another program or after the first handle has been destroyed, use `Database::open("./example-db", "example")`. Opening a database acquires an exclusive lock; concurrent database handles for the same directory are rejected.

`executeSql()` throws on failure. `tryExecuteSql()` returns `std::expected<QueryResult, QueryError>` for syntax, validation, constraint, and numeric errors. Storage and internal failures still throw. Rejected writes are rolled back before an expected error is returned.

`QueryResult` contains column names and types, rows of `Value` variants, an affected-row count, and a `returnsRows` flag. SQL NULL is represented by `std::monostate`.

## SQL support

| Area | Implemented functionality |
| --- | --- |
| Queries | `SELECT`, expressions, aliases, `*`, qualified columns, `WHERE` |
| Joins | Inner, left, right, full outer, and cross joins |
| Aggregation | `GROUP BY`, `HAVING`, `COUNT(*)`, `COUNT(expr)`, `SUM(expr)` |
| Expressions | Arithmetic, comparisons, `AND`, `OR`, `NOT`, `IS NULL`, `IS NOT NULL`, `ABS()` |
| Writes | `INSERT`, including multiple rows and omitted-column defaults; `UPDATE`; `DELETE` |
| Tables and indexes | `CREATE TABLE`, `CREATE INDEX`, `CREATE UNIQUE INDEX`, `DROP TABLE`, `DROP INDEX`, `TRUNCATE TABLE` |
| Constraints | Named constraints, primary keys, unique keys, foreign keys, NOT NULL, defaults, CHECK expressions, and composite keys |
| Schema changes | `ALTER TABLE ... ADD COLUMN`, `DROP COLUMN`, and `ADD CONSTRAINT ... CHECK`; other ALTER paths are still being completed |

Primary-key and UNIQUE constraints create backing unique indexes. Indexes support incremental updates, ordered cursors, and bulk construction. SQL query execution currently scans tables rather than selecting indexes through a query optimizer.

Supported SQL column types are `INT`/`INTEGER`, `BIGINT`, `DOUBLE`, `DECIMAL`/`NUMERIC`, `TEXT`/`VARCHAR`, `BOOLEAN`, `DATE`, and `DATETIMEOFFSET`/`DATETIME`.

- `DECIMAL` stores a signed 64-bit coefficient and a scale of up to 18, with checked arithmetic.
- `DATE` stores days since 1970-01-01.
- `DATETIMEOFFSET` stores UTC milliseconds and an offset in minutes. Comparisons use the UTC instant, so different offsets can represent equal values.
- A datetime without an offset uses the system time zone. Ambiguous or nonexistent local times are rejected; provide an explicit offset in those cases. Cross-type DATE/datetime comparisons interpret the date as local midnight.

For example, using the `users` table from the C++ example, submit these statements individually:

```sql
CREATE TABLE events (
    id INT PRIMARY KEY,
    user_id INT REFERENCES users(id),
    occurred DATETIMEOFFSET NOT NULL,
    amount DECIMAL CHECK (amount >= 0)
);

INSERT INTO events VALUES
    (1, 1, '2026-01-15 10:30:00+01:00', 12.50),
    (2, 1, '2026-01-15 09:30:00+00:00', 7.25);

SELECT u.name, e.amount
FROM users AS u LEFT JOIN events AS e ON u.id = e.user_id;

SELECT user_id, COUNT(*), SUM(amount)
FROM events
GROUP BY user_id
HAVING COUNT(*) > 1;

CREATE INDEX events_by_user ON events(user_id);
ALTER TABLE events ADD COLUMN label TEXT DEFAULT 'new';
ALTER TABLE events DROP COLUMN label;
```

`DROP TABLE ... CASCADE` keeps referencing tables and their rows, removing the dependent foreign keys. `DROP COLUMN ... CASCADE` follows the same principle for incoming references to the removed column. `TRUNCATE TABLE ... CASCADE` empties the target and its transitive referencing tables while preserving their schemas and index definitions.

## Storage and recovery

Each database is a directory:

```text
example-db/
├── db.meta             # Database identity, format version, checksum
├── tables/
│   └── users.table     # Table schema and data pages
├── indexes/
│   └── <name>.index    # Index metadata and B+ tree pages
└── journal/
    └── journal.log    # Present while a statement needs recovery
```

Tables and indexes use 4 KiB pages. Table rows use a NULL bitmap, fixed-width values, and a directory for variable-width values. A row is located by its page ID and slot index. Buffer managers cache pages, and page guards track pins and writes.

Writes use an undo journal containing file metadata and original page contents. Recovery records are made durable before the corresponding database changes are flushed. Failed statements roll back, and opening a database recovers an unfinished journal. The tests include injected I/O failures and simulated power-loss scenarios.

Adding or dropping a column currently reads the table's rows into memory, rewrites them using the new layout, and rebuilds affected indexes. Dropping a column also removes dependent local constraints and remaps surviving column references.

## Tests

CTest runs the test executables built by CMake. Build them before running CTest:

```sh
cmake --build build -j 2
ctest --test-dir build --output-on-failure
```

To rebuild and run the DDL tests alone:

```sh
cmake --build build --target db_ddl_test -j 2
ctest --test-dir build -R '^db_ddl_test$' --output-on-failure
```

Tests cover parsing and binding, expressions, numeric and temporal types, constraints, joins, grouping, indexes, page storage, DDL, rollback, and recovery. Power-loss tests exercise many simulated filesystem states and take longer than ordinary query tests.

## Project layout

| Files | Responsibility |
| --- | --- |
| `db_sql_lexer.*`, `db_sql_parser.*` | SQL tokens and syntax trees |
| `db_query_validator.*` | Name resolution, type checks, and bound statements |
| `db_expression_evaluator.*`, `db_query_executor.*` | Expression and query execution |
| `db_constraint_validator.*`, `db_row_validator.*` | Constraint and stored-row validation |
| `db_storage_engine.*`, `db_table.*` | Database operations and table cursors |
| `db_index.*`, `db_index_bulk.cpp`, `db_index_codec.*` | Index traversal, mutation, bulk construction, and encoding |
| `db_buffer_manager.*`, `db_page_file.*`, `db_read.*`, `db_write.*` | Cached pages and binary storage |
| `db_statement_recovery.*`, `db_journal_file.*`, `db_linux_file.*` | Recovery, journal records, and Linux file operations |
| [tests/](tests/) | Test executables and fixtures |

All implementation files listed above are under [src/](src/).

## Current limitations

- The SQL dialect currently lacks `ORDER BY`, `LIMIT`, `DISTINCT`, subqueries, views, triggers, and user-managed `BEGIN`/`COMMIT` transactions.
- Recovery operates at statement boundaries; there is no concurrent transaction or MVCC implementation.
- Query results and several intermediate operations are materialized in memory. Joins use nested loops.
- A stored row must fit on one data page, including its overhead; oversized rows are rejected.
- Function calls in stored CHECK and DEFAULT expressions are not supported. `AVG`, `MIN`, `MAX`, and other declared function IDs are not yet implemented as SQL functions.
- CREATE TABLE cannot currently bind a foreign key referencing the table being created.
- ALTER support is still evolving, particularly table renaming and backing-index cleanup when dropping constraints. `ALTER COLUMN ... SET/DROP NOT NULL` is not currently parsed.
- Type arguments such as `VARCHAR(n)` or `DECIMAL(p, s)` are parsed but are not enforced as length or precision declarations.
