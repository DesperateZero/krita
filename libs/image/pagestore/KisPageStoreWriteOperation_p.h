/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef KIS_PAGE_STORE_WRITE_OPERATION_P_H
#define KIS_PAGE_STORE_WRITE_OPERATION_P_H

#include <functional>
#include "KisPixelWriteCursor.h"

// Only the first two results permit selecting the compatibility route. Failed
// may follow executed pixel work: it must never trigger replay.
enum class KisPageStoreWriteOperationResult { Unavailable, Borrowed, Failed, Succeeded };
using KisPageStorePixelOperation = std::function<bool(KisPixelWriteCursor *)>;
using KisPageStoreWriteBoundary = bool (*)(const void *);

#endif
