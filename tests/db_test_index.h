#pragma once

#include "db_index.h"
#include "db_index_codec.h"
#include "db_read.h"

#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>

namespace index_test
{
    inline void require(bool condition, const std::string &message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    inline bool same(const LeafIndexEntry &a, const LeafIndexEntry &b)
    {
        return a.key.values == b.key.values && a.rowId == b.rowId;
    }

    struct Tree
    {
        PageFile file;
        IndexHeaderPage metadata;
        RowSchema schema;
        std::set<PageId> visited;
        std::vector<PageId> leaves;
        std::vector<LeafIndexEntry> entries;
        std::uint16_t height;
        bool allowDeletedKeys;

        Tree(const std::filesystem::path &root, const std::string &name, bool deletedKeys = false)
            : file(LinuxDirectory(root / "indexes"), name + ".index", LinuxFile::OpenMode::ReadOnly),
              metadata(std::get<IndexHeaderPage>(decodeIndexHeaderPage(file.readPage(0)).data)),
              schema{metadata.columns}, height(0), allowDeletedKeys(deletedKeys)
        {
            const auto page = decodeIndexPage(file.readPage(metadata.rootPageId), schema);
            height = std::get<IndexPage>(page.data).header.level;
            visit(metadata.rootPageId, height);
            require(visited.size() + 1 == metadata.nextUnusedPageId, "Unreachable or missing tree pages");
            require(file.size() == std::uint64_t{metadata.nextUnusedPageId} * PAGE_SIZE, "Incorrect index file length");
            for (std::size_t i = 0; i < leaves.size(); ++i)
            {
                const auto leaf = decodeIndexPage(file.readPage(leaves[i]), schema);
                require(leaf.nextPageId() == (i + 1 < leaves.size() ? leaves[i + 1] : 0), "Leaf sibling order is incorrect");
            }
            for (std::size_t i = 1; i < entries.size(); ++i)
                require(indexEntryLess(entries[i - 1], entries[i]), "Leaf order is not strict by key and RowId");
        }

        std::optional<LeafIndexEntry> visit(PageId id, std::uint16_t level)
        {
            require(id != 0 && id < metadata.nextUnusedPageId && visited.insert(id).second, "Invalid child or cycle");
            const auto page = decodeIndexPage(file.readPage(id), schema);
            require(page.pageId() == id, "Stored page ID mismatch");
            const auto &node = std::get<IndexPage>(page.data);
            require(node.header.level == level, "Unbalanced tree");
            if (level == 0)
            {
                leaves.push_back(id);
                const auto &values = std::get<std::vector<LeafIndexEntry>>(node.entries);
                require(allowDeletedKeys || !values.empty() || id == metadata.rootPageId, "Empty non-root leaf");
                entries.insert(entries.end(), values.begin(), values.end());
                if (values.empty()) return std::nullopt;
                return values.front();
            }
            const auto &values = std::get<std::vector<InternalIndexEntry>>(node.entries);
            require(!values.empty(), "Internal page has fewer than two children");
            const auto firstEntry = entries.size();
            auto minimum = visit(node.header.firstChildPageId, static_cast<std::uint16_t>(level - 1));
            std::optional<LeafIndexEntry> previousBoundary;
            for (const auto &separator : values)
            {
                const LeafIndexEntry boundary{separator.separatorKey, separator.separatorRowId};
                if (previousBoundary)
                    require(indexEntryLess(*previousBoundary, boundary), "Internal separators are not ordered");
                previousBoundary = boundary;
                if (entries.size() != firstEntry)
                    require(indexEntryLess(entries.back(), boundary), "Left subtree crosses separator boundary");
                auto childMin = visit(separator.pageId, static_cast<std::uint16_t>(level - 1));
                if (!allowDeletedKeys)
                    require(childMin && same(*childMin, boundary), "Separator is not the minimum of the right subtree");
                if (childMin)
                {
                    require(!indexEntryLess(*childMin, boundary), "Right subtree precedes separator boundary");
                    if (!minimum) minimum = childMin;
                }
            }
            return minimum;
        }

        // Route each stored entry using separators, independently of the leaf chain.
        void verifyRouting()
        {
            for (const auto &entry : entries)
            {
                auto id = metadata.rootPageId;
                for (;;)
                {
                    const auto page = decodeIndexPage(file.readPage(id), schema);
                    const auto &node = std::get<IndexPage>(page.data);
                    if (node.header.level == 0)
                    {
                        const auto &values = std::get<std::vector<LeafIndexEntry>>(node.entries);
                        require(std::ranges::any_of(values, [&](const auto &v) { return same(v, entry); }),
                                "Separator search routed to the wrong leaf");
                        break;
                    }
                    id = node.header.firstChildPageId;
                    for (const auto &separator : std::get<std::vector<InternalIndexEntry>>(node.entries))
                    {
                        if (indexEntryLess(entry, {separator.separatorKey, separator.separatorRowId})) break;
                        id = separator.pageId;
                    }
                }
            }
        }
    };

}
