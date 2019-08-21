// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2019 Bitcoin Association
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "legacy.h"

#include "amount.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "config.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "hash.h"
#include "net.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "script/standard.h"
#include "timedata.h"
#include "txmempool.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "validationinterface.h"
#include "versionbits.h"

#include <algorithm>
#include <queue>
#include <utility>

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>

using mining::BlockAssembler;
using mining::CBlockTemplate;

static const int MAX_COINBASE_SCRIPTSIG_SIZE = 100;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

int64_t UpdateTime(CBlockHeader *pblock, const Config &config,
                   const CBlockIndex *pindexPrev) {
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime =
        std::max(pindexPrev->GetMedianTimePast() + 1, GetAdjustedTime());

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    const Consensus::Params &consensusParams =
        config.GetChainParams().GetConsensus();

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, config);
    }

    return nNewTime - nOldTime;
}

LegacyBlockAssembler::LegacyBlockAssembler(const Config &_config)
: BlockAssembler{_config}
{
    if (gArgs.IsArgSet("-blockmintxfee")) {
        Amount n(0);
        ParseMoney(gArgs.GetArg("-blockmintxfee", ""), n);
        blockMinFeeRate = CFeeRate(n);
    } else {
        blockMinFeeRate = CFeeRate(DEFAULT_BLOCK_MIN_TX_FEE);
    }

    LOCK(cs_main);
    nMaxGeneratedBlockSize = ComputeMaxGeneratedBlockSize(chainActive.Tip());
}

void LegacyBlockAssembler::resetBlock() {
    inBlock.clear();

    // Reserve space for coinbase tx.
    nBlockSize = 1000;
    nBlockSigOps = 100;

    // These counters do not include coinbase tx.
    nBlockTx = 0;
    mBlockFees = Amount(0);

    lastFewTxs = 0;
    blockFinished = false;
}

static const std::vector<uint8_t>
getExcessiveBlockSizeSig(const Config &config) {
    std::string cbmsg = "/EB" + getSubVersionEB(config.GetMaxBlockSize()) + "/";
    const char *cbcstr = cbmsg.c_str();
    std::vector<uint8_t> vec(cbcstr, cbcstr + cbmsg.size());
    return vec;
}

std::unique_ptr<CBlockTemplate>
LegacyBlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn, CBlockIndex*& pindexPrev)
{
    int64_t nTimeStart = GetTimeMicros();

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());
    if (!pblocktemplate.get()) {
        return nullptr;
    }

    // Pointer for convenience.
    CBlockRef blockref = pblocktemplate->GetBlockRef();
    pblock = blockref.get();

    // Add dummy coinbase tx as first transaction.
    pblock->vtx.emplace_back();
    // updated at end
    pblocktemplate->vTxFees.push_back(Amount(-1));
    // updated at end
    pblocktemplate->vTxSigOpsCount.push_back(-1);

    LOCK2(cs_main, mempool.cs);
    CBlockIndex* pindexPrevNew = chainActive.Tip();
    nHeight = pindexPrevNew->nHeight + 1;

    nMaxGeneratedBlockSize = ComputeMaxGeneratedBlockSize(pindexPrevNew);

    nLockTimeCutoff =
        (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
            ? pindexPrevNew->GetMedianTimePast()
            : GetAdjustedTime();

    addPriorityTxs();
    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    addPackageTxs(nPackagesSelected, nDescendantsUpdated);

    int64_t nTime1 = GetTimeMicros();

    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;

    FillBlockHeader(blockref, pindexPrevNew, scriptPubKeyIn);

    pblocktemplate->vTxFees[0] = -1 * mBlockFees;
    pblocktemplate->vTxSigOpsCount[0] = GetSigOpCountWithoutP2SH(*pblock->vtx[0]);

    uint64_t nSerializeSize = GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION);
    LogPrintf("CreateNewBlock(): total size: %u txs: %u fees: %ld sigops %d\n",
              nSerializeSize, nBlockTx, mBlockFees, nBlockSigOps);

    // If required, check block validity
    int64_t nTimeValidationStart { GetTimeMicros() };
    if(mConfig.GetTestBlockCandidateValidity())
    {
        CValidationState state;
        BlockValidationOptions validationOptions { false, false, true };
        if (!TestBlockValidity(mConfig, state, *pblock, pindexPrevNew, validationOptions))
        {
            throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s",
                                               __func__, FormatStateMessage(state)));
        }
    }

    int64_t nTimeEnd = GetTimeMicros();
    LogPrint(
        BCLog::BENCH, "CreateNewBlock() packages: %.2fms (%d packages, %d "
                      "updated descendants), validity: %.2fms (total %.2fms)\n",
        0.001 * (nTime1 - nTimeStart), nPackagesSelected, nDescendantsUpdated,
        0.001 * (nTimeEnd - nTimeValidationStart), 0.001 * (nTimeEnd - nTimeStart));

    pindexPrev = pindexPrevNew;
    return std::move(pblocktemplate);
}

bool LegacyBlockAssembler::isStillDependent(CTxMemPool::txiter iter) {
    for (CTxMemPool::txiter parent : mempool.GetMemPoolParents(iter)) {
        if (!inBlock.count(parent)) {
            return true;
        }
    }
    return false;
}

void LegacyBlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries &testSet) {
    for (CTxMemPool::setEntries::iterator iit = testSet.begin();
         iit != testSet.end();) {
        // Only test txs not already in the block.
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool LegacyBlockAssembler::TestPackage(uint64_t packageSize, int64_t packageSigOps) {
    auto blockSizeWithPackage = nBlockSize + packageSize;
    if (blockSizeWithPackage >= nMaxGeneratedBlockSize) {
        return false;
    }
    if (nBlockSigOps + packageSigOps >=
        GetMaxBlockSigOpsCount(blockSizeWithPackage)) {
        return false;
    }
    return true;
}

/**
 * Perform transaction-level checks before adding to block:
 * - Transaction finality (locktime)
 * - Serialized size (in case -blockmaxsize is in use)
 */
bool LegacyBlockAssembler::TestPackageTransactions(
    const CTxMemPool::setEntries &package) {
    uint64_t nPotentialBlockSize = nBlockSize;
    for (const CTxMemPool::txiter it : package) {
        CValidationState state;
        if (!ContextualCheckTransaction(mConfig, it->GetTx(), state, nHeight,
                                        nLockTimeCutoff)) {
            return false;
        }

        uint64_t nTxSize =
            ::GetSerializeSize(it->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
        if (nPotentialBlockSize + nTxSize >= nMaxGeneratedBlockSize) {
            return false;
        }

        nPotentialBlockSize += nTxSize;
    }

    return true;
}

bool LegacyBlockAssembler::TestForBlock(CTxMemPool::txiter it) {
    auto blockSizeWithTx =
        nBlockSize +
        ::GetSerializeSize(it->GetTx(), SER_NETWORK, PROTOCOL_VERSION);
    if (blockSizeWithTx >= nMaxGeneratedBlockSize) {
        if (nBlockSize > nMaxGeneratedBlockSize - 100 || lastFewTxs > 50) {
            blockFinished = true;
            return false;
        }
        if (nBlockSize > nMaxGeneratedBlockSize - 1000) {
            lastFewTxs++;
        }
        return false;
    }

    auto maxBlockSigOps = GetMaxBlockSigOpsCount(blockSizeWithTx);
    if (nBlockSigOps + it->GetSigOpCount() >= maxBlockSigOps) {
        // If the block has room for no more sig ops then flag that the block is
        // finished.
        // TODO: We should consider adding another transaction that isn't very
        // dense in sigops instead of bailing out so easily.
        if (nBlockSigOps > maxBlockSigOps - 2) {
            blockFinished = true;
            return false;
        }
        // Otherwise attempt to find another tx with fewer sigops to put in the
        // block.
        return false;
    }

    // Must check that lock times are still valid. This can be removed once MTP
    // is always enforced as long as reorgs keep the mempool consistent.
    CValidationState state;
    if (!ContextualCheckTransaction(mConfig, it->GetTx(), state, nHeight,
                                    nLockTimeCutoff)) {
        return false;
    }

    return true;
}

void LegacyBlockAssembler::AddToBlock(CTxMemPool::txiter iter) {
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOpsCount.push_back(iter->GetSigOpCount());
    nBlockSize += iter->GetTxSize();
    ++nBlockTx;
    nBlockSigOps += iter->GetSigOpCount();
    mBlockFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority =
        gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        double dPriority = iter->GetPriority(nHeight);
        Amount dummy;
        mempool.ApplyDeltas(iter->GetTx().GetId(), dPriority, dummy);
        LogPrintf(
            "priority %.1f fee %s txid %s\n", dPriority,
            CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
            iter->GetTx().GetId().ToString());
    }
}

int LegacyBlockAssembler::UpdatePackagesForAdded(
    const CTxMemPool::setEntries &alreadyAdded,
    indexed_modified_transaction_set &mapModifiedTx) {
    int nDescendantsUpdated = 0;
    for (const CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set.
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCountWithAncestors -= it->GetSigOpCount();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
    return nDescendantsUpdated;
}

// Skip entries in mapTx that are already in a block or are present in
// mapModifiedTx (which implies that the mapTx ancestor state is stale due to
// ancestor inclusion in the block). Also skip transactions that we've already
// failed to add. This can happen if we consider a transaction in mapModifiedTx
// and it fails: we can then potentially consider it again while walking mapTx.
// It's currently guaranteed to fail again, but as a belt-and-suspenders check
// we put it in failedTx and avoid re-evaluation, since the re-evaluation would
// be using cached size/sigops/fee values that are not actually correct.
bool LegacyBlockAssembler::SkipMapTxEntry(
    CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx,
    CTxMemPool::setEntries &failedTx) {
    assert(it != mempool.mapTx.end());
    return mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it);
}

void LegacyBlockAssembler::SortForBlock(
    const CTxMemPool::setEntries &package, CTxMemPool::txiter entry,
    std::vector<CTxMemPool::txiter> &sortedEntries) {
    // Sort package by ancestor count. If a transaction A depends on transaction
    // B, then A's ancestor count must be greater than B's. So this is
    // sufficient to validly order the transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(),
              CompareTxIterByAncestorCount());
}

/**
 * addPackageTx includes transactions paying a fee by ensuring that
 * the partial ordering of transactions is maintained.  That is to say
 * children come after parents, despite having a potentially larger fee.
 * @param[out] nPackagesSelected    How many packages were selected
 * @param[out] nDescendantsUpdated  Number of descendant transactions updated
*/
void LegacyBlockAssembler::addPackageTxs(int &nPackagesSelected,
                                   int &nDescendantsUpdated) {

    // selection algorithm orders the mempool based on feerate of a
    // transaction including all unconfirmed ancestors. Since we don't remove
    // transactions from the mempool as we select them for block inclusion, we
    // need an alternate method of updating the feerate of a transaction with
    // its not-yet-selected ancestors as we go. This is accomplished by
    // walking the in-mempool descendants of selected transactions and storing
    // a temporary modified state in mapModifiedTxs. Each time through the
    // loop, we compare the best transaction in mapModifiedTxs with the next
    // transaction in the mempool to decide what transaction package to work
    // on next.

    // mapModifiedTx will store sorted packages after they are modified because
    // some of their txs are already in the block.
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work.
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors.
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator
        mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score>().end() ||
           !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score>().end() &&
            SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx,
                           failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry.
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                CompareModifiedEntry()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score than the one
                // from mapTx. Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        Amount packageFees = iter->GetModFeesWithAncestors();
        int64_t packageSigOps = iter->GetSigOpCountWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOps = modit->nSigOpCountWithAncestors;
        }

        if (packageFees < blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOps)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx, we
                // must erase failed entries so that we can consider the next
                // best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES &&
                nBlockSize > nMaxGeneratedBlockSize - 1000) {
                // Give up if we're close to full and haven't succeeded in a
                // while.
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit,
                                          nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final.
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, iter, sortedEntries);

        for (auto &entry : sortedEntries) {
            AddToBlock(entry);
            // Erase from the modified set, if present
            mapModifiedTx.erase(entry);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void LegacyBlockAssembler::addPriorityTxs() {
    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay.
    if (mConfig.GetBlockPriorityPercentage() == 0) {
        return;
    }

    uint64_t nBlockPrioritySize =
        nMaxGeneratedBlockSize * mConfig.GetBlockPriorityPercentage() / 100;

    // This vector will be sorted into a priority queue:
    std::vector<TxCoinAgePriority> vecPriority;
    TxCoinAgePriorityCompare pricomparer;
    std::map<CTxMemPool::txiter, double, CTxMemPool::CompareIteratorByHash>
        waitPriMap;
    typedef std::map<CTxMemPool::txiter, double,
                     CTxMemPool::CompareIteratorByHash>::iterator waitPriIter;
    double actualPriority = -1;

    vecPriority.reserve(mempool.mapTx.size());
    for (CTxMemPool::indexed_transaction_set::iterator mi =
             mempool.mapTx.begin();
         mi != mempool.mapTx.end(); ++mi) {
        double dPriority = mi->GetPriority(nHeight);
        Amount dummy;
        mempool.ApplyDeltas(mi->GetTx().GetId(), dPriority, dummy);
        vecPriority.push_back(TxCoinAgePriority(dPriority, mi));
    }
    std::make_heap(vecPriority.begin(), vecPriority.end(), pricomparer);

    CTxMemPool::txiter iter;

    // Add a tx from priority queue to fill the part of block reserved to
    // priority transactions.
    while (!vecPriority.empty() && !blockFinished) {
        iter = vecPriority.front().second;
        actualPriority = vecPriority.front().first;
        std::pop_heap(vecPriority.begin(), vecPriority.end(), pricomparer);
        vecPriority.pop_back();

        // If tx already in block, skip.
        if (inBlock.count(iter)) {
            // Shouldn't happen for priority txs.
            assert(false);
            continue;
        }

        // If tx is dependent on other mempool txs which haven't yet been
        // included then put it in the waitSet.
        if (isStillDependent(iter)) {
            waitPriMap.insert(std::make_pair(iter, actualPriority));
            continue;
        }

        // If this tx fits in the block add it, otherwise keep looping.
        if (TestForBlock(iter)) {
            AddToBlock(iter);

            // If now that this txs is added we've surpassed our desired
            // priority size or have dropped below the AllowFreeThreshold, then
            // we're done adding priority txs.
            if (nBlockSize >= nBlockPrioritySize ||
                !AllowFree(actualPriority)) {
                break;
            }

            // This tx was successfully added, so add transactions that depend
            // on this one to the priority queue to try again.
            for (CTxMemPool::txiter child : mempool.GetMemPoolChildren(iter)) {
                waitPriIter wpiter = waitPriMap.find(child);
                if (wpiter != waitPriMap.end()) {
                    vecPriority.push_back(
                        TxCoinAgePriority(wpiter->second, child));
                    std::push_heap(vecPriority.begin(), vecPriority.end(),
                                   pricomparer);
                    waitPriMap.erase(wpiter);
                }
            }
        }
    }
}

void IncrementExtraNonce(const Config &config, CBlock *pblock,
                         const CBlockIndex *pindexPrev,
                         unsigned int &nExtraNonce) {
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    // Height first in coinbase required for block.version=2
    unsigned int nHeight = pindexPrev->nHeight + 1;
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig =
        (CScript() << nHeight << CScriptNum(nExtraNonce)
                   << getExcessiveBlockSizeSig(config)) +
        COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= MAX_COINBASE_SCRIPTSIG_SIZE);

    pblock->vtx[0] = MakeTransactionRef(std::move(txCoinbase));
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}
