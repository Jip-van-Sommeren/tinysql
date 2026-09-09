#include "db_read.h"
#include "db_decimal.h"

#include <algorithm>
#include <bit>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <variant>

PageDecoder::PageDecoder(const RawPage &buffer)
    : buffer(buffer) {}

std::size_t PageDecoder::position() const
{
    return pos;
}

void PageDecoder::seek(std::size_t newPos)
{
    if (newPos > PAGE_SIZE)
    {
        throw std::runtime_error("seek past page boundary");
    }

    pos = newPos;
}

std::string PageDecoder::decodeString()
{
    std::uint32_t length = decodeUnsigned<std::uint32_t>();
    ensureAvailable(length);

    std::string result;
    result.reserve(length);

    for (std::uint32_t i = 0; i < length; ++i)
    {
        char c = static_cast<char>(
            std::to_integer<std::uint8_t>(buffer[pos + i]));

        result.push_back(c);
    }

    pos += length;
    return result;
}

void PageDecoder::decodeBytes(void *out, std::size_t size)
{
    ensureAvailable(size);

    auto *dest = static_cast<std::byte *>(out);

    std::copy(
        buffer.begin() + pos,
        buffer.begin() + pos + size,
        dest);

    pos += size;
}

std::vector<std::byte> PageDecoder::decodeBytes(std::size_t size)
{
    std::vector<std::byte> bytes(size);
    decodeBytes(bytes.data(), size);
    return bytes;
}

std::vector<std::byte> PageDecoder::decodeBytesAt(std::size_t offset, std::size_t size)
{
    std::size_t saved = position();
    seek(offset);

    std::vector<std::byte> bytes(size);
    decodeBytes(bytes.data(), size);

    seek(saved);
    return bytes;
}

void PageDecoder::ensureAvailable(std::size_t size) const
{
    if (pos + size > PAGE_SIZE)
    {
        throw std::runtime_error("read past page boundary");
    }
}

PageHeaderDecoder::PageHeaderDecoder(PageDecoder &decoder)
    : decoder(decoder) {}

PageHeader PageHeaderDecoder::decode()
{
    PageHeader header;
    header.pageId = decoder.decodeUnsignedAt<std::uint32_t>(PageHeaderLayout::PageId);

    std::uint8_t rawPageType =
        decoder.decodeUnsignedAt<std::uint8_t>(PageHeaderLayout::PageType);

    if (!isValidPageType(rawPageType))
    {
        throw std::runtime_error("Invalid page type: " + std::to_string(rawPageType));
    }

    header.pageType = static_cast<PageType>(rawPageType);
    header.slotCount = decoder.decodeUnsignedAt<std::uint16_t>(PageHeaderLayout::SlotCount);
    header.freeSpaceStart = decoder.decodeUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceStart);
    header.freeSpaceEnd = decoder.decodeUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceEnd);
    header.nextPageId = decoder.decodeUnsignedAt<std::uint32_t>(PageHeaderLayout::NextPageId);

    decoder.seek(PageHeaderLayout::Size);
    return header;
}

HeaderPageDecoder::HeaderPageDecoder(PageDecoder &decoder)
    : decoder(decoder), pageHeaderDecoder(decoder) {}

Page HeaderPageDecoder::decode()
{
    PageHeader pageHeader = pageHeaderDecoder.decode();

    if (pageHeader.pageType != PageType::HeaderPage)
    {
        throw std::runtime_error("Expected header page");
    }

    HeaderPage headerPage;
    headerPage.magic = decoder.decodeString();
    headerPage.version = decoder.decodeUnsigned<std::uint16_t>();
    headerPage.pageSize = decoder.decodeUnsigned<std::uint32_t>();
    headerPage.tableName = decoder.decodeString();

    std::uint32_t columnCount = decoder.decodeUnsigned<std::uint32_t>();
    headerPage.columns.reserve(columnCount);

    for (std::uint32_t i = 0; i < columnCount; ++i)
    {
        headerPage.columns.push_back(decodeColumn());
    }

    std::uint32_t constraintCount = decoder.decodeUnsigned<std::uint32_t>();
    headerPage.constraints.reserve(constraintCount);

    for (std::uint32_t i = 0; i < constraintCount; ++i)
    {
        headerPage.constraints.push_back(decodeConstraint());
    }

    headerPage.totalRowCount = decoder.decodeUnsigned<std::uint64_t>();
    headerPage.firstDataPageId = decoder.decodeUnsigned<std::uint32_t>();
    headerPage.lastDataPageId = decoder.decodeUnsigned<std::uint32_t>();
    headerPage.nextUnusedPageId = decoder.decodeUnsigned<std::uint32_t>();

    return Page{
        .header = pageHeader,
        .data = std::move(headerPage)};
}

Column HeaderPageDecoder::decodeColumn()
{
    Column column;
    column.name = decoder.decodeString();
    column.type = static_cast<DataType>(decoder.decodeUnsigned<std::uint8_t>());
    column.nullable = decoder.decodeUnsigned<std::uint8_t>() != 0;
    column.columnIndex = decoder.decodeUnsigned<std::uint32_t>();
    column.storage = decodeColumnStorage();

    return column;
}

ColumnStorage HeaderPageDecoder::decodeColumnStorage()
{
    auto kind = static_cast<ColumnStorageKind>(
        decoder.decodeUnsigned<std::uint8_t>());

    if (kind == ColumnStorageKind::Fixed)
    {
        return FixedColumnStorage{
            .offset = decoder.decodeUnsigned<std::uint32_t>(),
            .size = decoder.decodeUnsigned<std::uint32_t>()};
    }

    if (kind == ColumnStorageKind::Variable)
    {
        return VarColumnStorage{
            .varIndex = decoder.decodeUnsigned<std::uint32_t>()};
    }

    throw std::runtime_error("Unknown column storage type");
}

std::vector<ColumnId> HeaderPageDecoder::decodeColumnIds()
{
    std::uint32_t count = decoder.decodeUnsigned<std::uint32_t>();
    std::vector<ColumnId> columnIds;
    columnIds.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i)
    {
        columnIds.push_back(decoder.decodeUnsigned<std::uint32_t>());
    }

    return columnIds;
}

Constraint HeaderPageDecoder::decodeConstraint()
{
    ConstraintType type = static_cast<ConstraintType>(
        decoder.decodeUnsigned<std::uint8_t>());
    std::string name = decoder.decodeString();

    switch (type)
    {
    case ConstraintType::PrimaryKey:
        return BoundPrimaryKeyConstraintExpr{
            std::move(name),
            decodeColumnIds()};

    case ConstraintType::ForeignKey:
    {
        std::vector<ColumnId> localColumnIds = decodeColumnIds();
        std::string referencedTableName = decoder.decodeString();
        std::vector<ColumnId> referencedColumnIds = decodeColumnIds();

        return BoundForeignKeyConstraintExpr{
            std::move(name),
            std::move(localColumnIds),
            std::move(referencedTableName),
            std::move(referencedColumnIds)};
    }

    case ConstraintType::Unique:
        return BoundUniqueConstraintExpr{
            std::move(name),
            decodeColumnIds()};

    case ConstraintType::NotNull:
        return BoundNotNullConstraintExpr{
            std::move(name),
            decoder.decodeUnsigned<std::uint32_t>()};

    case ConstraintType::Null:
        return BoundNullConstraintExpr{
            std::move(name),
            decoder.decodeUnsigned<std::uint32_t>()};

    case ConstraintType::Default:
    {
        std::unique_ptr<BoundExpr> value = decodeExpression();
        ColumnId columnId = decoder.decodeUnsigned<std::uint32_t>();

        return BoundDefaultConstraintExpr{
            std::move(name),
            std::move(value),
            columnId};
    }

    case ConstraintType::Check:
        return BoundCheckConstraintExpr{
            std::move(name),
            decodeExpression()};

    }

    throw std::runtime_error("Unknown constraint type");
}

std::unique_ptr<BoundExpr> HeaderPageDecoder::decodeExpression()
{
    BoundExprKind kind = static_cast<BoundExprKind>(
        decoder.decodeUnsigned<std::uint8_t>());

    switch (kind)
    {
    case BoundExprKind::FunctionCall:
        throw std::runtime_error(
            "Stored function expressions are not supported");

    case BoundExprKind::ColumnReference:
    {
        ColumnId columnId = decoder.decodeUnsigned<std::uint32_t>();
        DataType type = static_cast<DataType>(
            decoder.decodeUnsigned<std::uint8_t>());
        return std::make_unique<BoundColumnExpr>(columnId, type);
    }

    case BoundExprKind::Literal:
    {
        DataType type = static_cast<DataType>(
            decoder.decodeUnsigned<std::uint8_t>());

        switch (type)
        {
        case DataType::Null:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::monostate{}},
                type);

        case DataType::Int:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::bit_cast<std::int32_t>(
                    decoder.decodeUnsigned<std::uint32_t>())},
                type);

        case DataType::BigInt:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::bit_cast<std::int64_t>(
                    decoder.decodeUnsigned<std::uint64_t>())},
                type);

        case DataType::Double:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::bit_cast<std::float64_t>(
                    decoder.decodeUnsigned<std::uint64_t>())},
                type);

        case DataType::Decimal:
        {
            const std::int64_t coefficient = std::bit_cast<std::int64_t>(
                decoder.decodeUnsigned<std::uint64_t>());
            const std::uint32_t scale = decoder.decodeUnsigned<std::uint32_t>();
            if (scale > MAX_DECIMAL_SCALE)
            {
                throw std::runtime_error(
                    "Stored DECIMAL literal scale exceeds 18 digits");
            }
            return std::make_unique<BoundLiteralExpr>(
                Value{DecimalValue{
                    .coefficient = coefficient,
                    .scale = scale}},
                type);
        }

        case DataType::Boolean:
        {
            const std::uint8_t raw = decoder.decodeUnsigned<std::uint8_t>();
            if (raw > 1)
            {
                throw std::runtime_error(
                    "Invalid BOOLEAN literal in stored expression");
            }
            return std::make_unique<BoundLiteralExpr>(Value{raw != 0}, type);
        }

        case DataType::Text:
            return std::make_unique<BoundLiteralExpr>(
                Value{decoder.decodeString()},
                type);

        default:
            throw std::runtime_error("Unsupported literal type in stored expression");
        }
    }

    case BoundExprKind::Binary:
    {
        BinaryOperator op = static_cast<BinaryOperator>(
            decoder.decodeUnsigned<std::uint8_t>());
        DataType resultType = static_cast<DataType>(
            decoder.decodeUnsigned<std::uint8_t>());
        std::unique_ptr<BoundExpr> left = decodeExpression();
        std::unique_ptr<BoundExpr> right = decodeExpression();

        return std::make_unique<BoundBinaryExpr>(
            op,
            std::move(left),
            std::move(right),
            resultType);
    }

    case BoundExprKind::Unary:
    {
        UnaryOperator op = static_cast<UnaryOperator>(
            decoder.decodeUnsigned<std::uint8_t>());
        DataType resultType = static_cast<DataType>(
            decoder.decodeUnsigned<std::uint8_t>());

        return std::make_unique<BoundUnaryExpr>(
            op,
            decodeExpression(),
            resultType);
    }

    case BoundExprKind::IsNull:
    {
        std::unique_ptr<BoundExpr> operand = decodeExpression();
        bool negated = decoder.decodeUnsigned<std::uint8_t>() != 0;
        return std::make_unique<BoundIsNullExpr>(
            std::move(operand),
            negated);
    }
    }

    throw std::runtime_error("Unknown stored expression type");
}

Value ValueDeserializer::decodeFixed(
    PageDecoder &decoder,
    std::size_t absoluteOffset,
    DataType type)
{
    switch (type)
    {
    case DataType::Int:
    {
        std::uint32_t raw =
            decoder.decodeUnsignedAt<std::uint32_t>(absoluteOffset);

        return std::bit_cast<std::int32_t>(raw);
    }

    case DataType::BigInt:
    {
        const std::uint64_t raw =
            decoder.decodeUnsignedAt<std::uint64_t>(absoluteOffset);
        return std::bit_cast<std::int64_t>(raw);
    }

    case DataType::Double:
    {
        const std::uint64_t raw =
            decoder.decodeUnsignedAt<std::uint64_t>(absoluteOffset);
        return std::bit_cast<std::float64_t>(raw);
    }

    case DataType::Decimal:
    {
        const std::uint64_t rawCoefficient =
            decoder.decodeUnsignedAt<std::uint64_t>(absoluteOffset);
        const std::uint32_t scale =
            decoder.decodeUnsignedAt<std::uint32_t>(
                absoluteOffset + sizeof(std::int64_t));
        if (scale > MAX_DECIMAL_SCALE)
        {
            throw std::runtime_error("Stored DECIMAL scale exceeds 18 digits");
        }
        return DecimalValue{
            .coefficient = std::bit_cast<std::int64_t>(rawCoefficient),
            .scale = scale};
    }

    case DataType::Boolean:
    {
        const std::uint8_t raw =
            decoder.decodeUnsignedAt<std::uint8_t>(absoluteOffset);
        if (raw > 1)
        {
            throw std::runtime_error("Invalid stored BOOLEAN value");
        }
        return raw != 0;
    }

    default:
        throw std::runtime_error("Unsupported fixed-width data type");
    }
}

Value ValueDeserializer::decodeVariable(
    PageDecoder &decoder,
    std::size_t absoluteOffset,
    std::uint32_t length,
    DataType type)
{
    switch (type)
    {
    case DataType::Text:
    {
        std::vector<std::byte> bytes =
            decoder.decodeBytesAt(absoluteOffset, length);

        const char *chars =
            reinterpret_cast<const char *>(bytes.data());

        return std::string(chars, chars + bytes.size());
    }

    default:
        throw std::runtime_error("Unsupported variable-width data type");
    }
}

RowDecoder::RowDecoder(
    PageDecoder &decoder,
    const HeaderPage &headerPage,
    std::size_t rowStart)
    : decoder(decoder),
      headerPage(headerPage),
      rowStart(rowStart) {}

Row RowDecoder::decodeRow()
{
    prepareLayout();

    std::vector<Value> values(headerPage.columns.size());

    for (const Column &column : headerPage.columns)
    {
        if (isNull(column.columnIndex))
        {
            values[column.columnIndex] = std::monostate{};
            continue;
        }

        if (std::holds_alternative<FixedColumnStorage>(column.storage))
        {
            const FixedColumnStorage &fixed =
                std::get<FixedColumnStorage>(column.storage);

            values[column.columnIndex] =
                decodeFixedValue(column, fixed);

            continue;
        }

        if (std::holds_alternative<VarColumnStorage>(column.storage))
        {
            const VarColumnStorage &var =
                std::get<VarColumnStorage>(column.storage);

            values[column.columnIndex] =
                decodeVariableValue(column, var);

            continue;
        }

        throw std::runtime_error("Unknown column storage type");
    }

    return Row{.values = std::move(values)};
}

void RowDecoder::prepareLayout()
{
    nullBitmapSizeBytes = (headerPage.columns.size() + 7) / 8;
    fixedAreaSize = 0;
    std::size_t varCount = 0;

    for (const Column &column : headerPage.columns)
    {
        if (std::holds_alternative<FixedColumnStorage>(column.storage))
        {
            const FixedColumnStorage &fixed =
                std::get<FixedColumnStorage>(column.storage);

            fixedAreaSize = std::max<std::size_t>(
                fixedAreaSize,
                fixed.offset + fixed.size);
        }
        else if (std::holds_alternative<VarColumnStorage>(column.storage))
        {
            ++varCount;
        }
        else
        {
            throw std::runtime_error("Unknown column storage type");
        }
    }

    fixedAreaStartOffset = nullBitmapSizeBytes;
    varDirStartOffset = fixedAreaStartOffset + fixedAreaSize;
    varDirSize = varCount * VarEntrySize;
    varDataStartOffset = varDirStartOffset + varDirSize;
}

bool RowDecoder::isNull(std::size_t columnIndex)
{
    std::size_t byteOffset = rowStart + (columnIndex / 8);
    std::size_t bitIndex = columnIndex % 8;

    std::uint8_t byte =
        decoder.decodeUnsignedAt<std::uint8_t>(byteOffset);

    return (byte & static_cast<std::uint8_t>(1u << bitIndex)) != 0;
}

Value RowDecoder::decodeFixedValue(
    const Column &column,
    const FixedColumnStorage &fixed)
{
    std::size_t absoluteOffset =
        rowStart + fixedAreaStartOffset + fixed.offset;

    return ValueDeserializer::decodeFixed(
        decoder,
        absoluteOffset,
        column.type);
}

Value RowDecoder::decodeVariableValue(
    const Column &column,
    const VarColumnStorage &var)
{
    std::size_t varEntryOffset =
        rowStart + varDirStartOffset + var.varIndex * VarEntrySize;

    std::uint32_t relativeOffset =
        decoder.decodeUnsignedAt<std::uint32_t>(varEntryOffset);

    std::uint32_t length =
        decoder.decodeUnsignedAt<std::uint32_t>(varEntryOffset + 4);

    std::size_t absoluteDataOffset =
        rowStart + relativeOffset;

    return ValueDeserializer::decodeVariable(
        decoder,
        absoluteDataOffset,
        length,
        column.type);
}

SlotDecoder::SlotDecoder(PageDecoder &decoder)
    : decoder(decoder) {}

Slot SlotDecoder::decodeSlot(std::uint16_t slotIndex) const
{
    std::size_t base = slotOffset(slotIndex);

    return Slot{
        .offset = decoder.decodeUnsignedAt<std::uint16_t>(base),
        .size = decoder.decodeUnsignedAt<std::uint16_t>(base + 2),
        .flags = decoder.decodeUnsignedAt<std::uint16_t>(base + 4)};
}

std::vector<Slot> SlotDecoder::decodeSlots(std::uint16_t slotCount) const
{
    std::vector<Slot> slots;
    slots.reserve(slotCount);

    for (std::uint16_t i = 0; i < slotCount; ++i)
    {
        slots.push_back(decodeSlot(i));
    }

    return slots;
}

std::size_t SlotDecoder::slotOffset(std::uint16_t slotIndex) const
{
    return PAGE_SIZE - ((static_cast<std::size_t>(slotIndex) + 1) * SlotSize);
}

DataPageDecoder::DataPageDecoder(PageDecoder &decoder, const HeaderPage &headerPage)
    : decoder(decoder),
      headerDecoder(decoder),
      slotDecoder(decoder),
      headerPage(headerPage) {}

Page DataPageDecoder::decode()
{
    PageHeader pageHeader = headerDecoder.decode();

    if (pageHeader.pageType != PageType::DataPage)
    {
        throw std::runtime_error("Expected data page");
    }

    DataPage dataPage;
    dataPage.slots = slotDecoder.decodeSlots(pageHeader.slotCount);
    dataPage.rows.reserve(pageHeader.slotCount);

    for (std::size_t i = 0; i < dataPage.slots.size(); ++i)
    {
        const Slot &slot = dataPage.slots[i];

        if (slot.has(SlotFlag::Deleted))
        {
            continue;
        }

        if (static_cast<std::size_t>(slot.offset) + slot.size > PAGE_SIZE)
        {
            throw std::runtime_error("slot points outside page");
        }

        RowDecoder rowDecoder{
            decoder,
            headerPage,
            slot.offset};

        Row row = rowDecoder.decodeRow();

        dataPage.rows.push_back(RowEntry{
            .slotIndex = static_cast<std::uint16_t>(i),
            .row = std::move(row)});
    }

    return Page{
        .header = pageHeader,
        .data = std::move(dataPage)};
}

RawPage readPageFromFile(
    const std::filesystem::path &tablePath,
    std::uint32_t pageId)
{
    RawPage page{};
    std::ifstream file{tablePath, std::ios::binary};

    if (!file)
    {
        throw std::runtime_error("Failed to open file: " + tablePath.string());
    }

    file.seekg(static_cast<std::streamoff>(pageId) * PAGE_SIZE);
    file.read(
        reinterpret_cast<char *>(page.data()),
        static_cast<std::streamsize>(page.size()));

    if (!file)
    {
        throw std::runtime_error("Failed to read page " + std::to_string(pageId));
    }

    return page;
}

bool isValidPageType(std::uint8_t value)
{
    return value == static_cast<std::uint8_t>(PageType::DataPage) ||
           value == static_cast<std::uint8_t>(PageType::IndexPage) ||
           value == static_cast<std::uint8_t>(PageType::OverflowPage) ||
           value == static_cast<std::uint8_t>(PageType::FreePage) ||
           value == static_cast<std::uint8_t>(PageType::HeaderPage);
}

PageHeader decodePageHeader(const RawPage &page)
{
    PageDecoder decoder(page);
    PageHeaderDecoder headerDecoder(decoder);
    return headerDecoder.decode();
}

Page decodeHeaderPage(const RawPage &page)
{
    PageDecoder decoder(page);
    HeaderPageDecoder headerDecoder(decoder);
    return headerDecoder.decode();
}

Page decodeDataPage(const RawPage &page, const HeaderPage &headerPage)
{
    PageDecoder decoder(page);
    DataPageDecoder dataPageDecoder(decoder, headerPage);
    return dataPageDecoder.decode();
}
