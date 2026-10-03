/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_SNAPSHOT_ARRAY_P_H
#define KIS_PAGE_SNAPSHOT_ARRAY_P_H

#include "KisMutationStorage_p.h"
#include <QVector>
#include <QSet>
#include <tuple>

// The public oracle keeps value/COW semantics. Its owned array and shared
// control now use the same real allocation policy as production projections.
// Qt conversion is only an export into a caller-owned compatibility value.
template<class T>
class KisPageSnapshotArray
{
    using Storage = std::vector<T, KisMutationStorageAllocator<T>>;
public:
    using value_type = T;
    using iterator = T *;
    using const_iterator = const T *;
    KisPageSnapshotArray() = default;
    KisPageSnapshotArray(std::initializer_list<T> values) { append(values.begin(), values.end()); }
    KisPageSnapshotArray(const QVector<T> &values) { append(values.cbegin(), values.cend()); }
    KisPageSnapshotArray(const QSet<T> &values) { append(values.cbegin(), values.cend()); }
    template<class Iterator> KisPageSnapshotArray(Iterator first, Iterator last) { append(first, last); }
    qsizetype size() const noexcept { return m_storage ? qsizetype(m_storage->size()) : 0; }
    qsizetype count() const noexcept { return size(); }
    qsizetype capacity() const noexcept { return m_storage ? qsizetype(m_storage->capacity()) : 0; }
    bool empty() const noexcept { return size() == 0; }
    bool isEmpty() const noexcept { return empty(); }
    const T *constData() const noexcept { return m_storage ? m_storage->data() : nullptr; }
    const T *data() const noexcept { return constData(); }
    T *data() { if (m_storage) detach(); return m_storage ? m_storage->data() : nullptr; }
    const_iterator begin() const noexcept { return constData(); }
    const_iterator end() const noexcept { return size() ? constData() + size() : constData(); }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }
    const_iterator constBegin() const noexcept { return begin(); }
    const_iterator constEnd() const noexcept { return end(); }
    iterator begin() { return data(); }
    iterator end() { auto *first = data(); return size() ? first + size() : first; }
    const T &operator[](qsizetype index) const { return m_storage->at(size_t(index)); }
    T &operator[](qsizetype index) { detach(); return m_storage->at(size_t(index)); }
    const T &at(qsizetype index) const { return (*this)[index]; }
    const T &first() const { return (*this)[0]; }
    T &first() { return (*this)[0]; }
    const T &last() const { return (*this)[size() - 1]; }
    T &last() { return (*this)[size() - 1]; }
    const T &front() const { return first(); }
    T &front() { return first(); }
    const T &back() const { return last(); }
    T &back() { return last(); }
    void clear() noexcept { m_storage.reset(); }
    void reserve(qsizetype count) { if (count > 0) { detach(); m_storage->reserve(size_t(count)); } }
    void resize(qsizetype count) { if (count || m_storage) { detach(); m_storage->resize(size_t(count)); } }
    void push_back(const T &value) { detach(); m_storage->push_back(value); }
    void push_back(T &&value) { detach(); m_storage->push_back(std::move(value)); }
    void append(const T &value) { push_back(value); }
    void append(T &&value) { push_back(std::move(value)); }
    template<class Iterator> void append(Iterator first, Iterator last)
    { if (first != last) { detach(); m_storage->insert(m_storage->end(), first, last); } }
    template<class Array, std::enable_if_t<std::is_same_v<std::decay_t<Array>, KisPageSnapshotArray>, int> = 0>
    void append(Array &&values) { auto copy = values; append(copy.cbegin(), copy.cend()); }
    bool contains(const T &value) const { return std::find(cbegin(), cend(), value) != cend(); }
    iterator erase(const_iterator position) { return erase(position, position + 1); }
    iterator erase(const_iterator first, const_iterator last)
    {
        const auto start = first == cbegin() ? 0 : first - cbegin();
        const auto finish = last == cbegin() ? 0 : last - cbegin();
        if (!m_storage) return nullptr;
        detach();
        m_storage->erase(m_storage->begin() + start, m_storage->begin() + finish);
        return m_storage->data() + start;
    }
    iterator insert(const_iterator position, const T &value)
    { const auto index = position == cbegin() ? 0 : position - cbegin(); detach(); m_storage->insert(m_storage->begin() + index, value); return m_storage->data() + index; }
    void insert(qsizetype index, const T &value) { detach(); m_storage->insert(m_storage->begin() + index, value); }
    void prepend(const T &value) { insert(qsizetype(0), value); }
    void removeAt(qsizetype index) { detach(); m_storage->erase(m_storage->begin() + index); }
    void removeLast() { detach(); m_storage->pop_back(); }
    void swap(KisPageSnapshotArray &other) noexcept { m_storage.swap(other.m_storage); }
    operator QVector<T>() const { return QVector<T>(cbegin(), cend()); }
    friend bool operator==(const KisPageSnapshotArray &a, const KisPageSnapshotArray &b)
    { return a.m_storage == b.m_storage || (a.size() == b.size() && std::equal(a.cbegin(), a.cend(), b.cbegin())); }
    friend bool operator!=(const KisPageSnapshotArray &a, const KisPageSnapshotArray &b) { return !(a == b); }
    friend bool operator==(const KisPageSnapshotArray &a, const QVector<T> &b)
    { return a.size() == b.size() && std::equal(a.cbegin(), a.cend(), b.cbegin()); }
    friend bool operator==(const QVector<T> &a, const KisPageSnapshotArray &b) { return b == a; }
    friend bool operator==(const KisPageSnapshotArray &a, const QSet<T> &b)
    { return a.size() == b.size() && std::all_of(a.cbegin(), a.cend(), [&](const T &value) { return b.contains(value); }); }
    friend bool operator==(const QSet<T> &a, const KisPageSnapshotArray &b) { return b == a; }
private:
    void detach()
    {
        if (!m_storage) m_storage = std::allocate_shared<Storage>(KisMutationStorageAllocator<Storage>{});
        else if (m_storage.use_count() != 1) m_storage = std::allocate_shared<Storage>(
            KisMutationStorageAllocator<Storage>{}, *m_storage);
    }
    std::shared_ptr<Storage> m_storage;
};

// Canonical exports are already ordered. Arbitrary compatibility inputs still
// get exact duplicate validation, without building a second key directory.
template<class Array, class Key>
bool kisPageArrayHasUniqueKeys(const Array &values, Key key)
{
    bool ordered = true;
    for (qsizetype i = 1; i < values.size(); ++i) {
        const auto before = key(values[i - 1]);
        const auto current = key(values[i]);
        if (before == current) return false;
        ordered &= before < current;
    }
    if (ordered) return true;
    for (qsizetype i = 1; i < values.size(); ++i)
        if (std::any_of(values.cbegin(), values.cbegin() + i,
            [&](const auto &value) { return key(value) == key(values[i]); })) return false;
    return true;
}

#endif
