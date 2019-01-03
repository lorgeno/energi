// Copyright (c) 2019-2019 The Energi Core developers
// Distributed under the MIT software license, see the accompanying
/* @flow */
// Copyright (c) 2012-2013 The PPCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/assign/list_of.hpp>
#include <boost/lexical_cast.hpp>

#include "chainparams.h"
#include "db.h"
#include "pos_kernel.h"
#include "script/interpreter.h"
#include "policy/policy.h"
#include "timedata.h"
#include "util.h"

using namespace std;

bool fTestNet = false; //Params().NetworkID() == CBaseChainParams::TESTNET;

// Modifier interval: time to elapse before new modifier is computed
// Set to 3-hour for production network and 20-minute for test network
unsigned int nModifierInterval;
int nStakeTargetSpacing = 60;
unsigned int getIntervalVersion(bool fTestNet)
{
    if (fTestNet)
        return MODIFIER_INTERVAL_TESTNET;
    else
        return MODIFIER_INTERVAL;
}

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("GetLastStakeModifier: null pindex");
    while (pindex && pindex->pprev && !pindex->IsGeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->IsGeneratedStakeModifier())
        return error("GetLastStakeModifier: no generation at genesis block");
    nStakeModifier = pindex->nStakeModifier();
    nModifierTime = pindex->GetBlockTime();
    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert(nSection >= 0 && nSection < 64);
    int64_t a = getIntervalVersion(fTestNet) * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1)));
    return a;
}

// Get stake modifier selection interval (in seconds)
static int64_t GetStakeModifierSelectionInterval()
{
    int64_t nSelectionInterval = 0;
    for (int nSection = 0; nSection < 64; nSection++) {
        nSelectionInterval += GetStakeModifierSelectionIntervalSection(nSection);
    }
    return nSelectionInterval;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(
    vector<pair<int64_t, uint256> >& vSortedByTimestamp,
    map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop,
    uint64_t nStakeModifierPrev,
    const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    arith_uint256 hashBest{0};
    *pindexSelected = nullptr;

    for (const auto & item : vSortedByTimestamp) {
        if (!mapBlockIndex.count(item.second))
            return error("SelectBlockFromCandidates: failed to find block index for candidate block %s", item.second.ToString().c_str());

        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;

        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;

        // compute the selection hash by hashing an input that is unique to that block
        uint256 hashProof = pindex->GetBlockHash();

        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        arith_uint256 hashSelection = UintToArith256(Hash(ss.begin(), ss.end()));

        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;

        if (fSelected && hashSelection < hashBest) {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        } else if (!fSelected) {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        }
    }

    LogPrint("debug", "%s: selection hash=%s\n", __func__, hashBest.ToString().c_str());

    return fSelected;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier)
{
    nStakeModifier = 0;
    if (!pindexPrev) {
        return true; // genesis block's modifier is 0
    }
    if (pindexPrev->nHeight == 0) {
        //Give a stake modifier to the first block
        nStakeModifier = 0x1234567887654321;
        return true;
    }

    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("ComputeNextStakeModifier: unable to get last modifier");

    LogPrint("stake", "%s: prev modifier=%08x time=%d\n",
             __func__, nStakeModifier, nModifierTime);

    if (nModifierTime / getIntervalVersion(fTestNet) >= pindexPrev->GetBlockTime() / getIntervalVersion(fTestNet))
        return true;

    // Sort candidate blocks by timestamp
    vector<pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * getIntervalVersion(fTestNet) / nStakeTargetSpacing);
    int64_t nSelectionInterval = GetStakeModifierSelectionInterval();
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / getIntervalVersion(fTestNet)) * getIntervalVersion(fTestNet) - nSelectionInterval;
    const CBlockIndex* pindex = pindexPrev;

    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart) {
        vSortedByTimestamp.push_back(make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }

    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound = 0; nRound < min(64, (int)vSortedByTimestamp.size()); nRound++) {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);

        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("%s: unable to select block at round %d", __func__, nRound);

        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);

        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(make_pair(pindex->GetBlockHash(), pindex));
        LogPrint("stake", "%s: selected round %d stop=%s height=%d bit=%d\n",
                 __func__, nRound, nSelectionIntervalStop,
                 pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (LogAcceptCategory("stake")) {
        string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate) {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const auto &item : mapSelectedBlocks) {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake() ? "S" : "W");
        }
        LogPrintf("%s: selection height [%d, %d] map %s\n", __func__, nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap.c_str());
    }
    
    LogPrint("stake", "%s: new modifier=%08x prevblktime=%d\n",
             __func__, nStakeModifierNew, pindexPrev->GetBlockTime());

    nStakeModifier = nStakeModifierNew;
    return true;
}

uint256 stakeHash(unsigned int nTimeTx, CDataStream ss, unsigned int prevoutIndex, uint256 prevoutHash, unsigned int nTimeBlockFrom)
{
    //Pivx will hash in the transaction hash and the index number in order to make sure each hash is unique
    ss << nTimeBlockFrom << prevoutIndex << prevoutHash << nTimeTx;
    return Hash(ss.begin(), ss.end());
}

//instead of looping outside and reinitializing variables many times, we will give a nTimeTx and also search interval so that we can do all the hashing here
bool CheckStakeKernelHash(unsigned int nBits, const CBlockIndex &blockFrom, const CTransaction txPrev, const COutPoint prevout, unsigned int& nTimeTx, unsigned int nHashDrift, bool fCheck, uint256& hashProofOfStake, uint64_t &nStakeModifier, bool fPrintProofOfStake)
{
    //assign new variables to make it easier to read
    CAmount nValueIn = txPrev.vout[prevout.n].nValue;
    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();
    auto min_age = Params().MinStakeAge();
    
    if (nValueIn < MIN_STAKE_AMOUNT) {
        return error("CheckStakeKernelHash() : stake value is too small %d < %d", nValueIn, MIN_STAKE_AMOUNT);
    }

    if (nTimeTx < nTimeBlockFrom) // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    if (nTimeBlockFrom + min_age > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation - nTimeBlockFrom=%d nStakeMinAge=%d nTimeTx=%d", nTimeBlockFrom, min_age, nTimeTx);

    //grab difficulty
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);
    arith_uint256 bnTarget = (arith_uint256(nValueIn) / 100) * bnTargetPerCoinDay;

    //grab stake modifier
    //-------------------
    uint64_t nRequiredStakeModifier = 0;
    
    if (!ComputeNextStakeModifier(&blockFrom, nRequiredStakeModifier)) {
        LogPrintf("CheckStakeKernelHash(): failed to get kernel stake modifier \n");
        return false;
    }
    
    if (fCheck) {
        if (nStakeModifier != nRequiredStakeModifier) {
            return error(
                "%s : nStakeModifier mismatch at %d %llu != %llu",
                __func__, blockFrom.nHeight,
                nStakeModifier, nRequiredStakeModifier );
        }
    } else {
        nStakeModifier = nRequiredStakeModifier;
    }

    //create data stream once instead of repeating it in the loop
    CDataStream ss(SER_GETHASH, 0);
    ss << nStakeModifier;

    // if wallet is simply checking to make sure a hash is valid
    //-------------------
    if (fCheck) {
        uint256 requiredHashProofOfStake = stakeHash(nTimeTx, ss, prevout.n, prevout.hash, nTimeBlockFrom);
        
        if (requiredHashProofOfStake != hashProofOfStake) {
            return error(
                "%s : nStakeModifier mismatch at %d:%llu:%d:%s:%d %s != %s",
                __func__,
                nTimeTx, nStakeModifier, prevout.n, prevout.hash.ToString().c_str(), nTimeBlockFrom,
                hashProofOfStake.ToString().c_str(),
                requiredHashProofOfStake.ToString().c_str() );
        }

        return UintToArith256(hashProofOfStake) < bnTarget;
    }

    // search
    //-------------------
    bool fSuccess = false;
    unsigned int nTryTime = 0;

    for (auto i = 0U; i < nHashDrift; ++i)
    {
        //hash this iteration
        nTryTime = nTimeTx + i;
        hashProofOfStake = stakeHash(nTryTime, ss, prevout.n, prevout.hash, nTimeBlockFrom);

        // if stake hash does not meet the target then continue to next iteration
        if (UintToArith256(hashProofOfStake) >= bnTarget) {
            continue;
        }

        fSuccess = true; // if we make it this far then we have successfully created a stake hash
        nTimeTx = nTryTime;

        if (fDebug || fPrintProofOfStake) {
            LogPrintf("CheckStakeKernelHash() : using modifier %s at height=%d timestamp=%s for block from height=%d timestamp=%s\n",
                boost::lexical_cast<std::string>(nStakeModifier).c_str(),
                blockFrom.nHeight,
                DateTimeStrFormat("%Y-%m-%d %H:%M:%S", blockFrom.nTime).c_str(),
                blockFrom.nHeight,
                DateTimeStrFormat("%Y-%m-%d %H:%M:%S", blockFrom.GetBlockTime()).c_str());
            LogPrintf("CheckStakeKernelHash() : pass protocol=%s modifier=%s nTimeBlockFrom=%u prevoutHash=%s nTimeTxPrev=%u nPrevout=%u nTimeTx=%u hashProof=%s\n",
                "0.3",
                boost::lexical_cast<std::string>(nStakeModifier).c_str(),
                nTimeBlockFrom, prevout.hash.ToString().c_str(), nTimeBlockFrom, prevout.n, nTryTime,
                hashProofOfStake.ToString().c_str());
        }
        break;
    }

    return fSuccess;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(const CBlockHeader &block)
{
    auto block_hash = block.GetHash();

    if (block.posBlockSig.empty()) {
        if (fDebug) {
            error("%s : the block is not signed: %s", __func__, block_hash.ToString().c_str());
        }

        return false;
    }

    auto &consensus = Params().GetConsensus();

    COutPoint prevout = block.StakeInput();

    // First try finding the previous transaction in database
    uint256 txinHashBlock;
    CTransactionRef txinPrevRef;
    CBlockIndex* pindex_tx = nullptr;

    if (!GetTransaction(prevout.hash, txinPrevRef, consensus, txinHashBlock, true))
        return error("CheckProofOfStake() : INFO: read txPrev failed");

    // Check tx input block is known
    {
        BlockMap::iterator it = mapBlockIndex.find(txinHashBlock);
        if (it != mapBlockIndex.end())
            pindex_tx = it->second;
        else
            return error("CheckProofOfStake() : unknowns take block");
    }

    // Extract stake public key ID and verify block signature
    {
        txnouttype whichType;
        std::vector<std::vector<unsigned char>> vSolutions;
        CKeyID key_id;
        const auto &spk = txinPrevRef->vout[prevout.n].scriptPubKey;

        if (!Solver(spk, whichType, vSolutions)) {
            return error("%s : invalid stake input script for block %s", __func__,
                         block_hash.ToString().c_str());
        }

        if (whichType == TX_PUBKEYHASH) // pay to address type
        {
            key_id = CKeyID(uint160(vSolutions[0]));
        }
        else if (whichType == TX_PUBKEY) // pay to public key
        {
            key_id = CPubKey(vSolutions[0]).GetID();
        }
        else
        {
            return error("%s : not supported stake type %d for block %s", __func__,
                         block_hash.ToString().c_str());
        }

        if (!block.CheckBlockSignature(key_id)) {
            return error("%s : failed block signature: %s", __func__, block_hash.ToString().c_str());
        }
    }

    unsigned int nInterval = 0;
    unsigned int nTime = block.nTime;
    uint256 hashProofOfStake = block.hashProofOfStake();
    uint64_t nStakeModifier = block.nStakeModifier();
    
    bool is_valid = CheckStakeKernelHash(
            block.nBits,
            *pindex_tx, *txinPrevRef, prevout,
            nTime, nInterval, true,
            hashProofOfStake, nStakeModifier,
            fDebug);

    if (!is_valid) {
        return error(
            "CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s \n",
            prevout.hash.ToString().c_str(), hashProofOfStake.ToString().c_str());
    }

    return true;
}
