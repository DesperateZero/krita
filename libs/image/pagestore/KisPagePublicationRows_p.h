/* SPDX-FileCopyrightText: 2026 Krita contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_PAGE_PUBLICATION_ROWS_P_H
#define KIS_PAGE_PUBLICATION_ROWS_P_H
#include <cstddef>
#include <iterator>
struct KisPagePublicationChange;
// Original producer link. Intent-only entries carry this pointer, not a full
// acceptance workspace. Native storage belongs to the actual page record;
// semantic-only rows belong to the original mutation's deletion batch.
struct KisPagePublicationSource {
    KisPagePublicationChange *publicationRow = nullptr;
};

// Non-owning, constant-time row access. Native rows stay in the original
// mutation's stable slots; external inputs stay in their candidate's storage.
// The owner freezes the directory for the complete prepare/accept/cleanup.
class KisPagePublicationRows
{
public:
    class Iterator
    {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = KisPagePublicationChange;
        using difference_type = ptrdiff_t;
        using reference = value_type &;
        using pointer = value_type *;
        Iterator() = default;
        Iterator(KisPagePublicationChange *direct, KisPagePublicationSource *const *indirect, ptrdiff_t index)
            : direct(direct), indirect(indirect), index(index) {}
        reference operator*() const;
        pointer operator->() const { return &**this; }
        reference operator[](ptrdiff_t n) const { return *(*this + n); }
        Iterator &operator++() { ++index; return *this; }
        Iterator operator++(int) { auto old = *this; ++*this; return old; }
        Iterator &operator--() { --index; return *this; }
        Iterator operator--(int) { auto old = *this; --*this; return old; }
        Iterator &operator+=(ptrdiff_t n) { index += n; return *this; }
        Iterator &operator-=(ptrdiff_t n) { index -= n; return *this; }
        friend Iterator operator+(Iterator it, ptrdiff_t n) { return it += n; }
        friend Iterator operator+(ptrdiff_t n, Iterator it) { return it += n; }
        friend Iterator operator-(Iterator it, ptrdiff_t n) { return it -= n; }
        ptrdiff_t operator-(const Iterator &other) const { return index - other.index; }
        bool operator==(const Iterator &other) const
        { return direct == other.direct && indirect == other.indirect && index == other.index; }
        bool operator!=(const Iterator &other) const { return !(*this == other); }
        bool operator<(const Iterator &other) const { return index < other.index; }
        bool operator>(const Iterator &other) const { return other < *this; }
        bool operator<=(const Iterator &other) const { return !(other < *this); }
        bool operator>=(const Iterator &other) const { return !(*this < other); }
    private:
        KisPagePublicationChange *direct = nullptr;
        KisPagePublicationSource *const *indirect = nullptr;
        ptrdiff_t index = 0;
    };
    using const_iterator = Iterator;
    KisPagePublicationRows() = default;
    KisPagePublicationRows(KisPagePublicationChange *rows, size_t count) : direct(rows), count(count) {}
    KisPagePublicationRows(KisPagePublicationSource *const *rows, size_t count) : indirect(rows), count(count) {}
    explicit operator bool() const { return direct || indirect; }
    size_t size() const { return count; }
    bool empty() const { return !count; }
    Iterator begin() const { return {direct, indirect, 0}; }
    Iterator end() const { return {direct, indirect, ptrdiff_t(count)}; }
    Iterator cbegin() const { return begin(); }
    Iterator cend() const { return end(); }
    std::reverse_iterator<Iterator> rbegin() const { return std::reverse_iterator<Iterator>(end()); }
    std::reverse_iterator<Iterator> rend() const { return std::reverse_iterator<Iterator>(begin()); }
    KisPagePublicationChange &operator[](size_t i) const { return *(begin() + ptrdiff_t(i)); }
    KisPagePublicationChange &at(size_t i) const { return (*this)[i]; }
    KisPagePublicationRows subspan(size_t offset) const;
private:
    KisPagePublicationChange *direct = nullptr;
    KisPagePublicationSource *const *indirect = nullptr;
    size_t count = 0;
};
#endif
