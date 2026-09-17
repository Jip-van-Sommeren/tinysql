#include "db_read.h"
#include "db_decimal.h"
#include "db_page_file.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <variant>

void ByteReader::seek(std::size_t newPos)
{
    ensureRange(newPos, 0);
    pos = newPos;
}

std::string ByteReader::readString()
{
    const auto length = readUnsignedAt<std::uint32_t>(pos);
    const std::size_t start = pos + sizeof(std::uint32_t);
    ensureRange(start, length);

    std::string result;
    if (length != 0)
    {
        result.assign(
            reinterpret_cast<const char *>(buffer.data() + start), length);
    }

    pos = start + length;
    return result;
}

void ByteReader::readBytes(void *out, std::size_t count)
{
    ensureRange(pos, count);
    if (count == 0)
    {
        return;
    }
    if (out == nullptr)
    {
        throw std::invalid_argument("ByteReader: null destination");
    }

    std::memmove(out, buffer.data() + pos, count);
    pos += count;
}

std::vector<std::byte> ByteReader::readBytes(std::size_t count)
{
    auto result = readBytesAt(pos, count);
    pos += count;
    return result;
}

std::vector<std::byte> ByteReader::readBytesAt(
    std::size_t offset, std::size_t count) const
{
    ensureRange(offset, count);
    std::vector<std::byte> result(count);
    if (count != 0)
    {
        std::memcpy(result.data(), buffer.data() + offset, count);
    }
    return result;
}

void ByteReader::ensureRange(std::size_t offset, std::size_t count) const
{
    if (offset > buffer.size() || count > buffer.size() - offset)
    {
        throw std::out_of_range("ByteReader: buffer bounds exceeded");
    }
}

PageHeaderDecoder::PageHeaderDecoder(ByteReader &decoder)
    : decoder(decoder) {}

PageHeader PageHeaderDecoder::decode()
{
    PageHeader header;
    header.pageId = decoder.readUnsignedAt<std::uint32_t>(PageHeaderLayout::PageId);

    std::uint8_t rawPageType =
        decoder.readUnsignedAt<std::uint8_t>(PageHeaderLayout::PageType);

    if (!isValidPageType(rawPageType))
    {
        throw std::runtime_error("Invalid page type: " + std::to_string(rawPageType));
    }

    header.pageType = static_cast<PageType>(rawPageType);
    header.slotCount = decoder.readUnsignedAt<std::uint16_t>(PageHeaderLayout::SlotCount);
    header.freeSpaceStart = decoder.readUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceStart);
    header.freeSpaceEnd = decoder.readUnsignedAt<std::uint16_t>(PageHeaderLayout::FreeSpaceEnd);
    header.nextPageId = decoder.readUnsignedAt<std::uint32_t>(PageHeaderLayout::NextPageId);

    decoder.seek(PageHeaderLayout::Size);
    return header;
}

HeaderPageDecoder::HeaderPageDecoder(ByteReader &decoder)
    : decoder(decoder), pageHeaderDecoder(decoder) {}

Page HeaderPageDecoder::decode()
{
    PageHeader pageHeader = pageHeaderDecoder.decode();

    if (pageHeader.pageType != PageType::HeaderPage)
    {
        throw std::runtime_error("Expected header page");
    }

    HeaderPage headerPage;
    headerPage.magic = decoder.readString();
    headerPage.version = decoder.readUnsigned<std::uint16_t>();
    headerPage.pageSize = decoder.readUnsigned<std::uint32_t>();
    headerPage.tableName = decoder.readString();

    std::uint32_t columnCount = decoder.readUnsigned<std::uint32_t>();
    headerPage.columns.reserve(columnCount);

    for (std::uint32_t i = 0; i < columnCount; ++i)
    {
        headerPage.columns.push_back(decodeColumn());
    }

    std::uint32_t constraintCount = decoder.readUnsigned<std::uint32_t>();
    headerPage.constraints.reserve(constraintCount);

    for (std::uint32_t i = 0; i < constraintCount; ++i)
    {
        headerPage.constraints.push_back(decodeConstraint());
    }

    headerPage.totalRowCount = decoder.readUnsigned<std::uint64_t>();
    headerPage.firstDataPageId = decoder.readUnsigned<std::uint32_t>();
    headerPage.lastDataPageId = decoder.readUnsigned<std::uint32_t>();
    headerPage.nextUnusedPageId = decoder.readUnsigned<std::uint32_t>();

    return Page{
        .header = pageHeader,
        .data = std::move(headerPage)};
}

Column HeaderPageDecoder::decodeColumn()
{
    Column column;
    column.name = decoder.readString();
    column.type = static_cast<DataType>(decoder.readUnsigned<std::uint8_t>());
    column.nullable = decoder.readUnsigned<std::uint8_t>() != 0;
    column.columnIndex = decoder.readUnsigned<std::uint32_t>();
    column.storage = decodeColumnStorage();

    return column;
}

ColumnStorage HeaderPageDecoder::decodeColumnStorage()
{
    auto kind = static_cast<ColumnStorageKind>(
        decoder.readUnsigned<std::uint8_t>());

    if (kind == ColumnStorageKind::Fixed)
    {
        return FixedColumnStorage{
            .offset = decoder.readUnsigned<std::uint32_t>(),
            .size = decoder.readUnsigned<std::uint32_t>()};
    }

    if (kind == ColumnStorageKind::Variable)
    {
        return VarColumnStorage{
            .varIndex = decoder.readUnsigned<std::uint32_t>()};
    }

    throw std::runtime_error("Unknown column storage type");
}

std::vector<ColumnId> HeaderPageDecoder::decodeColumnIds()
{
    std::uint32_t count = decoder.readUnsigned<std::uint32_t>();
    std::vector<ColumnId> columnIds;
    columnIds.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i)
    {
        columnIds.push_back(decoder.readUnsigned<std::uint32_t>());
    }

    return columnIds;
}

Constraint HeaderPageDecoder::decodeConstraint()
{
    ConstraintType type = static_cast<ConstraintType>(
        decoder.readUnsigned<std::uint8_t>());
    std::string name = decoder.readString();

    switch (type)
    {
    case ConstraintType::PrimaryKey:
        return BoundPrimaryKeyConstraintExpr{
            std::move(name),
            decodeColumnIds()};

    case ConstraintType::ForeignKey:
    {
        std::vector<ColumnId> localColumnIds = decodeColumnIds();
        std::string referencedTableName = decoder.readString();
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
            decoder.readUnsigned<std::uint32_t>()};

    case ConstraintType::Null:
        return BoundNullConstraintExpr{
            std::move(name),
            decoder.readUnsigned<std::uint32_t>()};

    case ConstraintType::Default:
    {
        std::unique_ptr<BoundExpr> value = decodeExpression();
        ColumnId columnId = decoder.readUnsigned<std::uint32_t>();

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
        decoder.readUnsigned<std::uint8_t>());

    switch (kind)
    {
    case BoundExprKind::FunctionCall:
        throw std::runtime_error(
            "Stored function expressions are not supported");

    case BoundExprKind::ColumnReference:
    {
        ColumnId columnId = decoder.readUnsigned<std::uint32_t>();
        DataType type = static_cast<DataType>(
            decoder.readUnsigned<std::uint8_t>());
        return std::make_unique<BoundColumnExpr>(columnId, type);
    }

    case BoundExprKind::Literal:
    {
        DataType type = static_cast<DataType>(
            decoder.readUnsigned<std::uint8_t>());

        switch (type)
        {
        case DataType::Null:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::monostate{}},
                type);

        case DataType::Int:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::bit_cast<std::int32_t>(
                    decoder.readUnsigned<std::uint32_t>())},
                type);

        case DataType::BigInt:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::bit_cast<std::int64_t>(
                    decoder.readUnsigned<std::uint64_t>())},
                type);

        case DataType::Double:
            return std::make_unique<BoundLiteralExpr>(
                Value{std::bit_cast<std::float64_t>(
                    decoder.readUnsigned<std::uint64_t>())},
                type);

        case DataType::Decimal:
        {
            const std::int64_t coefficient = std::bit_cast<std::int64_t>(
                decoder.readUnsigned<std::uint64_t>());
            const std::uint32_t scale = decoder.readUnsigned<std::uint32_t>();
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
            const std::uint8_t raw = decoder.readUnsigned<std::uint8_t>();
            if (raw > 1)
            {
                throw std::runtime_error(
                    "Invalid BOOLEAN literal in stored expression");
            }
            return std::make_unique<BoundLiteralExpr>(Value{raw != 0}, type);
        }

        case DataType::Text:
            return std::make_unique<BoundLiteralExpr>(
                Value{decoder.readString()},
                type);

        default:
            throw std::runtime_error("Unsupported literal type in stored expression");
        }
    }

    case BoundExprKind::Binary:
    {
        BinaryOperator op = static_cast<BinaryOperator>(
            decoder.readUnsigned<std::uint8_t>());
        DataType resultType = static_cast<DataType>(
            decoder.readUnsigned<std::uint8_t>());
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
            decoder.readUnsigned<std::uint8_t>());
        DataType resultType = static_cast<DataType>(
            decoder.readUnsigned<std::uint8_t>());

        return std::make_unique<BoundUnaryExpr>(
            op,
            decodeExpression(),
            resultType);
    }

    case BoundExprKind::IsNull:
    {
        std::unique_ptr<BoundExpr> operand = decodeExpression();
        bool negated = decoder.readUnsigned<std::uint8_t>() != 0;
        return std::make_unique<BoundIsNullExpr>(
            std::move(operand),
            negated);
    }
    }

    throw std::runtime_error("Unknown stored expression type");
}

Value ValueDeserializer::decodeFixed(
    ByteReader &decoder,
    std::size_t absoluteOffset,
    DataType type)
{
    switch (type)
    {
    case DataType::Int:
    {
        std::uint32_t raw =
            decoder.readUnsignedAt<std::uint32_t>(absoluteOffset);

        return std::bit_cast<std::int32_t>(raw);
    }

    case DataType::BigInt:
    {
        const std::uint64_t raw =
            decoder.readUnsignedAt<std::uint64_t>(absoluteOffset);
        return std::bit_cast<std::int64_t>(raw);
    }

    case DataType::Double:
    {
        const std::uint64_t raw =
            decoder.readUnsignedAt<std::uint64_t>(absoluteOffset);
        return std::bit_cast<std::float64_t>(raw);
    }

    case DataType::Decimal:
    {
        const std::uint64_t rawCoefficient =
            decoder.readUnsignedAt<std::uint64_t>(absoluteOffset);
        const std::uint32_t scale =
            decoder.readUnsignedAt<std::uint32_t>(
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
            decoder.readUnsignedAt<std::uint8_t>(absoluteOffset);
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
    ByteReader &decoder,
    std::size_t absoluteOffset,
    std::uint32_t length,
    DataType type)
{
    switch (type)
    {
    case DataType::Text:
    {
        std::vector<std::byte> bytes =
            decoder.readBytesAt(absoluteOffset, length);

        const char *chars =
            reinterpret_cast<const char *>(bytes.data());

        return std::string(chars, chars + bytes.size());
    }

    default:
        throw std::runtime_error("Unsupported variable-width data type");
    }
}

RowDecoder::RowDecoder(
    ByteReader &decoder,
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
        decoder.readUnsignedAt<std::uint8_t>(byteOffset);

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
        decoder.readUnsignedAt<std::uint32_t>(varEntryOffset);

    std::uint32_t length =
        decoder.readUnsignedAt<std::uint32_t>(varEntryOffset + 4);

    std::size_t absoluteDataOffset =
        rowStart + relativeOffset;

    return ValueDeserializer::decodeVariable(
        decoder,
        absoluteDataOffset,
        length,
        column.type);
}

SlotDecoder::SlotDecoder(ByteReader &decoder)
    : decoder(decoder) {}

Slot SlotDecoder::decodeSlot(std::uint16_t slotIndex) const
{
    std::size_t base = slotOffset(slotIndex);

    return Slot{
        .offset = decoder.readUnsignedAt<std::uint16_t>(base),
        .size = decoder.readUnsignedAt<std::uint16_t>(base + 2),
        .flags = decoder.readUnsignedAt<std::uint16_t>(base + 4)};
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

DataPageDecoder::DataPageDecoder(ByteReader &decoder, const HeaderPage &headerPage)
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
    PageFile file{tablePath, LinuxFile::OpenMode::ReadOnly};
    return file.readPage(pageId);
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
    ByteReader decoder(page.bytes);
    PageHeaderDecoder headerDecoder(decoder);
    return headerDecoder.decode();
}

Page decodeHeaderPage(const RawPage &page)
{
    ByteReader decoder(page.bytes);
    HeaderPageDecoder headerPageDecoder(decoder);
    return headerPageDecoder.decode();
}

Page decodeDataPage(const RawPage &page, const HeaderPage &headerPage)
{
    ByteReader decoder(page.bytes);
    DataPageDecoder dataPageDecoder(decoder, headerPage);
    return dataPageDecoder.decode();
}
