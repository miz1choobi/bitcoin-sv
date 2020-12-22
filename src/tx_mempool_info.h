// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#pragma once

#include "amount.h"
#include "primitives/transaction.h"
#include "txn_validation_data.h"

#include <mutex>
#include <variant>

#include <boost/noncopyable.hpp>

class CAsyncMempoolTxDB;
class CTxMemPoolEntry;
class CMempoolTxDBReader;

/**
 * Wrapper for on-disk transactions.
 * Once the transaction is moved to disk, further uses of transaction will
 * bring it in memory as a transient copy for that user only. The wrapper will
 * not store the reference.
 */
class CTransactionWrapper : boost::noncopyable {
public:
    CTransactionWrapper(const CTransactionRef &tx,
                        const std::shared_ptr<CMempoolTxDBReader>& txDB);
    CTransactionWrapper(const TxId &txid,
                        const std::shared_ptr<CMempoolTxDBReader>& txDB);

    CTransactionRef GetTx() const;
    const TxId& GetId() const noexcept;
    bool IsInMemory() const noexcept;
    TxStorage GetTxStorage() const noexcept
    {
        return (IsInMemory() ? TxStorage::memory : TxStorage::txdb);
    }
    bool HasDatabase(const std::shared_ptr<CMempoolTxDBReader>& txDB) const noexcept;

    void ResetTransaction();

private:
    const TxId txid;
    const std::shared_ptr<CMempoolTxDBReader> mempoolTxDB;

    // Documentation typedefs
    using OwnedPtr = CTransactionRef;
    using WeakPtr = CWeakTransactionRef;

    // Must be mutable so that accessors can be const.
    mutable std::mutex guard;
    mutable std::variant<OwnedPtr, WeakPtr> txref;

    // Accessor for the CAsyncMempoolTxDB worker thread.
    friend class CAsyncMempoolTxDB;
    CTransactionRef GetInMemoryTx();
};

using CTransactionWrapperRef = std::shared_ptr<CTransactionWrapper>;

/**
 * Information about a mempool transaction.
 */
struct TxMempoolInfo
{
    explicit TxMempoolInfo() = default;
    explicit TxMempoolInfo(const CTxMemPoolEntry& entry);
    TxMempoolInfo(const CTransactionRef& ptx,
                  const std::shared_ptr<CMempoolTxDBReader>& txdb = {nullptr});

    bool IsNull() const;

    const TxId& GetTxId() const;

    const CTransactionRef& GetTx() const;

    TxStorage GetTxStorage() const noexcept;

    /** Time the transaction entered the mempool. */
    int64_t nTime {0};

    /** Feerate of the transaction. */
    CFeeRate feeRate {};

    /** The fee delta. */
    Amount nFeeDelta {};

    /** size of the serialized transaction */
    size_t nTxSize {};

private:
    /** The transaction wrapper */
    CTransactionWrapperRef wrapper {nullptr};

    // A local cache for the transaction which may be stored on disk in the
    // mempool transaction database and we don't want to re-read it every time
    // we need a reference.
    mutable CTransactionRef tx {nullptr};

    static const TxId nullTxId;
};

