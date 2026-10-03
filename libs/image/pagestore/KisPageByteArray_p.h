/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_BYTE_ARRAY_P_H
#define KIS_PAGE_BYTE_ARRAY_P_H

#include "KisMutationStorage_p.h"
#include <QByteArray>
#include <QDataStream>
#include <QDebug>
#include <cstring>

// Format values share immutable, actually funded bytes through roots,
// descriptors and source handles. Qt views borrow those bytes synchronously;
// a mutation first prepares an independent paid copy.
class KisPageByteArray
{
    using Bytes = std::vector<char, KisMutationStorageAllocator<char>>;
public:
    KisPageByteArray() = default;
    KisPageByteArray(const char *value) { assign(value, qsizetype(std::strlen(value))); }
    KisPageByteArray(const char *value, qsizetype size) { assign(value, size); }
    KisPageByteArray(const QByteArray &value) { assign(value.constData(), value.size()); }
    KisPageByteArray &operator=(const QByteArray &value)
    { assign(value.constData(), value.size()); return *this; }
    KisPageByteArray &operator=(const char *value)
    { assign(value, qsizetype(std::strlen(value))); return *this; }
    qsizetype size() const noexcept { return m_bytes ? qsizetype(m_bytes->size() - 1) : 0; }
    bool isEmpty() const noexcept { return size() == 0; }
    const char *constData() const noexcept { return m_bytes ? m_bytes->data() : ""; }
    void clear() noexcept { m_bytes.reset(); }
    void fill(char value)
    {
        if (!m_bytes) return;
        detach();
        std::fill_n(m_bytes->data(), size_t(size()), value);
    }
    char operator[](qsizetype index) const { Q_ASSERT(index >= 0 && index < size()); return (*m_bytes)[size_t(index)]; }
    char &operator[](qsizetype index)
    { Q_ASSERT(index >= 0 && index < size()); detach(); return (*m_bytes)[size_t(index)]; }
    QByteArray view() const noexcept { return QByteArray::fromRawData(constData(), size()); }
    // A returned Qt value must own its bytes if it escapes the format owner.
    operator QByteArray() const { return QByteArray(constData(), size()); }
    friend bool operator==(const KisPageByteArray &a, const KisPageByteArray &b)
    { return a.m_bytes == b.m_bytes || (a.size() == b.size() && std::equal(a.constData(), a.constData() + a.size(), b.constData())); }
    friend bool operator!=(const KisPageByteArray &a, const KisPageByteArray &b) { return !(a == b); }
    friend bool operator==(const KisPageByteArray &a, const QByteArray &b) { return a.view() == b; }
    friend bool operator==(const QByteArray &a, const KisPageByteArray &b) { return a == b.view(); }
    friend bool operator!=(const KisPageByteArray &a, const QByteArray &b) { return !(a == b); }
    friend bool operator!=(const QByteArray &a, const KisPageByteArray &b) { return !(a == b); }
    friend QDebug operator<<(QDebug stream, const KisPageByteArray &value) { return stream << value.view(); }
    friend QDataStream &operator<<(QDataStream &stream, const KisPageByteArray &value)
    { return stream << value.view(); }
    friend QDataStream &operator>>(QDataStream &stream, KisPageByteArray &value)
    {
        QByteArray input;
        stream >> input;
        value = input;
        return stream;
    }
private:
    void assign(const char *data, qsizetype size)
    {
        if (!size) { clear(); return; }
        auto bytes = std::allocate_shared<Bytes>(KisMutationStorageAllocator<Bytes>{});
        bytes->reserve(size_t(size) + 1);
        bytes->insert(bytes->end(), data, data + size);
        bytes->push_back('\0');
        m_bytes = std::move(bytes);
    }
    void detach()
    {
        if (m_bytes.use_count() != 1) assign(constData(), size());
    }
    std::shared_ptr<Bytes> m_bytes;
};

#endif
