// Copyright (c) 2014-2019 The vds Core developers
// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "chain.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "coins.h"
#include "consensus/validation.h"
#include "validation.h"
#include "primitives/transaction.h"
#include "rpc/server.h"
#include "streams.h"
#include "sync.h"
#include "txdb.h"
#include "txmempool.h"
#include "util.h"
#include "utilstrencodings.h"
#include "hash.h"
#include <key_io.h>
#include <stdint.h>

#include <univalue.h>

#include <boost/thread/thread.hpp> // boost::thread::interrupt
#include <regex>

#include <mutex>
#include <condition_variable>
using namespace std;

extern void TxToJSON(const CTransaction& tx, const uint256 hashBlock, UniValue& entry);
void ScriptPubKeyToJSON(const CScript& scriptPubKey, UniValue& out, bool fIncludeHex);

double GetDifficultyINTERNAL(const CBlockIndex* blockindex)
{
    // Floating point number that is a multiple of the minimum difficulty,
    // minimum difficulty = 1.0.
    if (blockindex == NULL) {
        if (chainActive.Tip() == NULL)
            return 1.0;
        else
            blockindex = chainActive.Tip();
    }

    uint32_t bits = blockindex->nBits;

    uint32_t powLimit =
        UintToArith256(Params().GetConsensus().powLimit).GetCompact();
    int nShift = (bits >> 24) & 0xff;
    int nShiftAmount = (powLimit >> 24) & 0xff;

    double dDiff =
        (double) (powLimit & 0x00ffffff) /
        (double) (bits & 0x00ffffff);

    while (nShift < nShiftAmount) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > nShiftAmount) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

double GetDifficulty(const CBlockIndex* blockindex)
{
    return GetDifficultyINTERNAL(blockindex);
}

double GetNetworkDifficulty(const CBlockIndex* blockindex)
{
    return GetDifficultyINTERNAL(blockindex);
}

static UniValue ValuePoolDesc(
    const std::string& name,
    const boost::optional<CAmount> chainValue,
    const boost::optional<CAmount> valueDelta)
{
    UniValue rv(UniValue::VOBJ);
    rv.push_back(Pair("id", name));
    rv.push_back(Pair("monitored", (bool)chainValue));
    if (chainValue) {
        rv.push_back(Pair("chainValue", ValueFromAmount(*chainValue)));
        rv.push_back(Pair("chainValueZat", *chainValue));
    }
    if (valueDelta) {
        rv.push_back(Pair("valueDelta", ValueFromAmount(*valueDelta)));
        rv.push_back(Pair("valueDeltaZat", *valueDelta));
    }
    return rv;
}

UniValue blockheaderToJSON(const CBlockIndex* blockindex)
{
    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hash", blockindex->GetBlockHash().GetHex()));
    int confirmations = -1;
    // Only report confirmations if the block is on the main chain
    if (chainActive.Contains(blockindex))
        confirmations = chainActive.Height() - blockindex->nHeight + 1;
    result.push_back(Pair("confirmations", confirmations));
    result.push_back(Pair("height", blockindex->nHeight));
    result.push_back(Pair("version", blockindex->nVersion));
    result.push_back(Pair("versionHex", strprintf("%08x", blockindex->nVersion)));
    result.push_back(Pair("merkleroot", blockindex->hashMerkleRoot.GetHex()));
    result.push_back(Pair("finalsaplingroot", blockindex->hashFinalSaplingRoot.GetHex()));
    result.push_back(Pair("hashstateroot", blockindex->hashStateRoot.GetHex()));
    result.push_back(Pair("hashutxoroot", blockindex->hashUTXORoot.GetHex()));
    result.push_back(Pair("solution", HexStr(blockindex->nSolution)));
    result.push_back(Pair("vibpool", blockindex->nVibPool));
    result.push_back(Pair("debttandia", blockindex->nDebtTandia));
    result.push_back(Pair("time", (int64_t) blockindex->nTime));
    result.push_back(Pair("nonce", blockindex->nNonce.GetHex()));
    result.push_back(Pair("bits", strprintf("%08x", blockindex->nBits)));
    result.push_back(Pair("difficulty", GetDifficulty(blockindex)));
    result.push_back(Pair("chainwork", blockindex->nChainWork.GetHex()));

    if (blockindex->pprev)
        result.push_back(Pair("previousblockhash", blockindex->pprev->GetBlockHash().GetHex()));
    CBlockIndex* pnext = chainActive.Next(blockindex);
    if (pnext)
        result.push_back(Pair("nextblockhash", pnext->GetBlockHash().GetHex()));
    return result;
}

UniValue blockToJSON(const CBlock& block, const CBlockIndex* blockindex, bool txDetails = false)
{
    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hash", block.GetHash().GetHex()));
    int confirmations = -1;
    // Only report confirmations if the block is on the main chain
    if (chainActive.Contains(blockindex))
        confirmations = chainActive.Height() - blockindex->nHeight + 1;
    result.push_back(Pair("confirmations", confirmations));
    result.push_back(Pair("size", (int) ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION)));
    result.push_back(Pair("height", blockindex->nHeight));
    result.push_back(Pair("version", block.nVersion));
    result.push_back(Pair("versionHex", strprintf("%08x", block.nVersion)));
    result.push_back(Pair("merkleroot", block.hashMerkleRoot.GetHex()));
    result.push_back(Pair("finalsaplingroot", block.hashFinalSaplingRoot.GetHex()));
    UniValue txs(UniValue::VARR);
    for (const auto& tx : block.vtx) {
        if (txDetails) {
            UniValue objTx(UniValue::VOBJ);
            TxToJSON(*tx, uint256(), objTx);
            txs.push_back(objTx);
        } else
            txs.push_back(tx->GetHash().GetHex());
    }
    result.push_back(Pair("tx", txs));
    result.push_back(Pair("chaincluetxes", (int64_t)(blockindex->nChainClueTx)));
    result.push_back(Pair("clueleft", blockindex->nClueLeft));
    result.push_back(Pair("vibpool", block.nVibPool));
    result.push_back(Pair("debttandia", blockindex->nDebtTandia));
    result.push_back(Pair("time", block.GetBlockTime()));
    result.push_back(Pair("hashstateroot", block.hashStateRoot.GetHex()));
    result.push_back(Pair("hashutxoroot", block.hashUTXORoot.GetHex()));
    result.push_back(Pair("bits", strprintf("%08x", block.nBits)));
    result.push_back(Pair("nonce", block.nNonce.GetHex()));
    result.push_back(Pair("solution", HexStr(block.nSolution)));
    result.push_back(Pair("difficulty", GetDifficulty(blockindex)));
    result.push_back(Pair("powhash", block.GetPoWHash().GetHex()));
    result.push_back(Pair("chainwork", blockindex->nChainWork.GetHex()));

    UniValue valuePools(UniValue::VARR);
    valuePools.push_back(ValuePoolDesc("sapling", blockindex->nChainSaplingValue, blockindex->nSaplingValue));
    result.push_back(Pair("valuePools", valuePools));

    if (blockindex->pprev)
        result.push_back(Pair("previousblockhash", blockindex->pprev->GetBlockHash().GetHex()));
    CBlockIndex* pnext = chainActive.Next(blockindex);
    if (pnext)
        result.push_back(Pair("nextblockhash", pnext->GetBlockHash().GetHex()));
    return result;
}

//////////////////////////////////////////////////////////////////////////// // qtum

UniValue executionResultToJSON(const dev::eth::ExecutionResult& exRes)
{
    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("gasUsed", CAmount(exRes.gasUsed)));
    std::stringstream ss;
    ss << exRes.excepted;
    result.push_back(Pair("excepted", ss.str()));
    result.push_back(Pair("newAddress", exRes.newAddress.hex()));
    result.push_back(Pair("output", HexStr(exRes.output)));
    result.push_back(Pair("codeDeposit", static_cast<int32_t> (exRes.codeDeposit)));
    result.push_back(Pair("gasRefunded", CAmount(exRes.gasRefunded)));
    result.push_back(Pair("depositSize", static_cast<int32_t> (exRes.depositSize)));
    result.push_back(Pair("gasForDeposit", CAmount(exRes.gasForDeposit)));
    return result;
}

UniValue transactionReceiptToJSON(const dev::eth::TransactionReceipt& txRec)
{
    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("stateRoot", txRec.stateRoot().hex()));
    result.push_back(Pair("gasUsed", CAmount(txRec.gasUsed())));
    result.push_back(Pair("bloom", txRec.bloom().hex()));
    UniValue logEntries(UniValue::VARR);
    dev::eth::LogEntries logs = txRec.log();
    for (dev::eth::LogEntry log : logs) {
        UniValue logEntrie(UniValue::VOBJ);
        logEntrie.push_back(Pair("address", log.address.hex()));
        UniValue topics(UniValue::VARR);
        for (dev::h256 l : log.topics) {
            topics.push_back(l.hex());
        }
        logEntrie.push_back(Pair("topics", topics));
        logEntrie.push_back(Pair("data", HexStr(log.data)));
        logEntries.push_back(logEntrie);
    }
    result.push_back(Pair("log", logEntries));
    return result;
}
////////////////////////////////////////////////////////////////////////////

UniValue getblockcount(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getblockcount\n"
            "\nReturns the number of blocks in the longest blockchain.\n"
            "\nResult:\n"
            "n    (numeric) The current block count\n"
            "\nExamples:\n"
            + HelpExampleCli("getblockcount", "")
            + HelpExampleRpc("getblockcount", "")
        );

    LOCK(cs_main);
    return chainActive.Height();
}

UniValue getbestblockhash(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getbestblockhash\n"
            "\nReturns the hash of the best (tip) block in the longest blockchain.\n"
            "\nResult:\n"
            "\"hex\"      (string) the block hash hex encoded\n"
            "\nExamples:\n"
            + HelpExampleCli("getbestblockhash", "")
            + HelpExampleRpc("getbestblockhash", "")
        );

    LOCK(cs_main);
    return chainActive.Tip()->GetBlockHash().GetHex();
}

void RPCNotifyBlockChange(bool ibd, const CBlockIndex* pindex)
{
    if (pindex) {
        std::lock_guard<std::mutex> lock(cs_blockchange);
        latestblock.hash = pindex->GetBlockHash();
        latestblock.height = pindex->nHeight;
    }
    cond_blockchange.notify_all();
}

UniValue getdifficulty(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getdifficulty\n"
            "\nReturns the proof-of-work difficulty as a multiple of the minimum difficulty.\n"
            "\nResult:\n"
            "n.nnn       (numeric) the proof-of-work difficulty as a multiple of the minimum difficulty.\n"
            "\nExamples:\n"
            + HelpExampleCli("getdifficulty", "")
            + HelpExampleRpc("getdifficulty", "")
        );

    LOCK(cs_main);
    return GetNetworkDifficulty();
}

void entryToJSON(UniValue& info, const CTxMemPoolEntry& e)
{
    AssertLockHeld(mempool.cs);

    info.push_back(Pair("size", (int)e.GetTxSize()));
    info.push_back(Pair("fee", ValueFromAmount(e.GetFee())));
    info.push_back(Pair("modifiedfee", ValueFromAmount(e.GetModifiedFee())));
    info.push_back(Pair("time", e.GetTime()));
    info.push_back(Pair("height", (int)e.GetHeight()));
    info.push_back(Pair("descendantcount", e.GetCountWithDescendants()));
    info.push_back(Pair("descendantsize", e.GetSizeWithDescendants()));
    info.push_back(Pair("descendantfees", e.GetModFeesWithDescendants()));
    info.push_back(Pair("ancestorcount", e.GetCountWithAncestors()));
    info.push_back(Pair("ancestorsize", e.GetSizeWithAncestors()));
    info.push_back(Pair("ancestorfees", e.GetModFeesWithAncestors()));
    info.push_back(Pair("wtxid", mempool.vTxHashes[e.vTxHashesIdx].first.ToString()));
    const CTransaction& tx = e.GetTx();
    std::set<std::string> setDepends;
    for (const CTxIn& txin : tx.vin) {
        if (mempool.exists(txin.prevout.hash))
            setDepends.insert(txin.prevout.hash.ToString());
    }

    UniValue depends(UniValue::VARR);
    for (const std::string& dep : setDepends) {
        depends.push_back(dep);
    }

    info.push_back(Pair("depends", depends));
}

UniValue mempoolToJSON(bool fVerbose)
{
    if (fVerbose) {
        LOCK(mempool.cs);
        UniValue o(UniValue::VOBJ);
        for (const CTxMemPoolEntry& e : mempool.mapTx) {
            const uint256& hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(info, e);
            o.push_back(Pair(hash.ToString(), info));
        }
        return o;
    } else {
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);

        UniValue a(UniValue::VARR);
        for (const uint256& hash : vtxid)
            a.push_back(hash.ToString());

        return a;
    }
}

UniValue getrawmempool(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
            "getrawmempool ( verbose )\n"
            "\nReturns all transaction ids in memory pool as a json array of string transaction ids.\n"
            "\nArguments:\n"
            "1. verbose (boolean, optional, default=false) True for a json object, false for array of transaction ids\n"
            "\nResult: (for verbose = false):\n"
            "[                     (json array of string)\n"
            "  \"transactionid\"     (string) The transaction id\n"
            "  ,...\n"
            "]\n"
            "\nResult: (for verbose = true):\n"
            "{                           (json object)\n"
            "  \"transactionid\" : {       (json object)\n"
            "    \"size\" : n,             (numeric) transaction size in bytes\n"
            "    \"fee\" : n,              (numeric) transaction fee in bitcoins\n"
            "    \"time\" : n,             (numeric) local time transaction entered pool in seconds since 1 Jan 1970 GMT\n"
            "    \"height\" : n,           (numeric) block height when transaction entered pool\n"
            "    \"startingpriority\" : n, (numeric) priority when transaction entered pool\n"
            "    \"currentpriority\" : n,  (numeric) transaction priority now\n"
            "    \"depends\" : [           (array) unconfirmed transactions used as inputs for this transaction\n"
            "        \"transactionid\",    (string) parent transaction id\n"
            "       ... ]\n"
            "  }, ...\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getrawmempool", "true")
            + HelpExampleRpc("getrawmempool", "true")
        );

    bool fVerbose = false;
    if (request.params.size() > 0)
        fVerbose = request.params[0].get_bool();

    return mempoolToJSON(fVerbose);
}

UniValue getblockhash(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "getblockhash height\n"
            "\nReturns hash of block in best-block-chain at height provided.\n"
            "\nArguments:\n"
            "1. height         (numeric, required) The height index\n"
            "\nResult:\n"
            "\"hash\"         (string) The block hash\n"
            "\nExamples:\n"
            + HelpExampleCli("getblockhash", "1000")
            + HelpExampleRpc("getblockhash", "1000")
        );

    LOCK(cs_main);

    int nHeight = request.params[0].get_int();
    if (nHeight < 0 || nHeight > chainActive.Height())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");

    CBlockIndex* pblockindex = chainActive[nHeight];
    return pblockindex->GetBlockHash().GetHex();
}

UniValue getblockheader(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "getblockheader \"hash\" ( verbose )\n"
            "\nIf verbose is false, returns a string that is serialized, hex-encoded data for blockheader 'hash'.\n"
            "If verbose is true, returns an Object with information about blockheader <hash>.\n"
            "\nArguments:\n"
            "1. \"hash\"          (string, required) The block hash\n"
            "2. verbose           (boolean, optional, default=true) true for a json object, false for the hex encoded data\n"
            "\nResult (for verbose = true):\n"
            "{\n"
            "  \"hash\" : \"hash\",     (string) the block hash (same as provided)\n"
            "  \"confirmations\" : n,   (numeric) The number of confirmations, or -1 if the block is not on the main chain\n"
            "  \"height\" : n,          (numeric) The block height or index\n"
            "  \"version\" : n,         (numeric) The block version\n"
            "  \"versionHex\" : \"00000000\", (string) The block version formatted in hexadecimal\n"
            "  \"merkleroot\" : \"xxxx\", (string) The merkle root\n"
            "  \"time\" : ttt,          (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"nonce\" : n,           (numeric) The nonce\n"
            "  \"bits\" : \"1d00ffff\", (string) The bits\n"
            "  \"difficulty\" : x.xxx,  (numeric) The difficulty\n"
            "  \"previousblockhash\" : \"hash\",  (string) The hash of the previous block\n"
            "  \"nextblockhash\" : \"hash\",      (string) The hash of the next block\n"
            "}\n"
            "\nResult (for verbose=false):\n"
            "\"data\"             (string) A string that is serialized, hex-encoded data for block 'hash'.\n"
            "\nExamples:\n"
            + HelpExampleCli("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblockheader", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
        );

    LOCK(cs_main);

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));

    bool fVerbose = true;
    if (request.params.size() > 1)
        fVerbose = request.params[1].get_bool();

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlockIndex* pblockindex = mapBlockIndex[hash];

    if (!fVerbose) {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << pblockindex->GetBlockHeader();
        std::string strHex = HexStr(ssBlock.begin(), ssBlock.end());
        return strHex;
    }

    return blockheaderToJSON(pblockindex);
}

UniValue getblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw runtime_error(
            "getblock \"hash|height\" ( verbose )\n"
            "\nIf verbose is false, returns a string that is serialized, hex-encoded data for block 'hash|height'.\n"
            "If verbose is true, returns an Object with information about block <hash|height>.\n"
            "\nArguments:\n"
            "1. \"hash|height\"     (string, required) The block hash or height\n"
            "2. verbose                (boolean, optional, default=true) true for a json object, false for the hex encoded data\n"
            "\nResult (for verbose = true):\n"
            "{\n"
            "  \"hash\" : \"hash\",     (string) the block hash (same as provided)\n"
            "  \"confirmations\" : n,   (numeric) The number of confirmations, or -1 if the block is not on the main chain\n"
            "  \"size\" : n,            (numeric) The block size\n"
            "  \"height\" : n,          (numeric) The block height or index\n"
            "  \"version\" : n,         (numeric) The block version\n"
            "  \"versionHex\" : \"00000000\", (string) The block version formatted in hexadecimal\n"
            "  \"merkleroot\" : \"xxxx\", (string) The merkle root\n"
            "  \"tx\" : [               (array of string) The transaction ids\n"
            "     \"transactionid\"     (string) The transaction id\n"
            "     ,...\n"
            "  ],\n"
            "  \"time\" : ttt,          (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"nonce\" : n,           (numeric) The nonce\n"
            "  \"bits\" : \"1d00ffff\", (string) The bits\n"
            "  \"difficulty\" : x.xxx,  (numeric) The difficulty\n"
            "  \"previousblockhash\" : \"hash\",  (string) The hash of the previous block\n"
            "  \"nextblockhash\" : \"hash\"       (string) The hash of the next block\n"
            "}\n"
            "\nResult (for verbose=false):\n"
            "\"data\"             (string) A string that is serialized, hex-encoded data for block 'hash'.\n"
            "\nExamples:\n"
            + HelpExampleCli("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleRpc("getblock", "\"00000000c937983704a73af28acdec37b049d214adbda81d7e2a3dd146f6ed09\"")
            + HelpExampleCli("getblock", "12800")
            + HelpExampleRpc("getblock", "12800")
        );

    LOCK(cs_main);

    std::string strHash = request.params[0].get_str();

    // If height is supplied, find the hash
    if (strHash.size() < (2 * sizeof (uint256))) {
        int nHeight = -1;
        try {
            nHeight = std::stoi(strHash);
        } catch (const std::exception& e) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid block height parameter");
        }

        if (nHeight < 0 || nHeight > chainActive.Height()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");
        }
        strHash = chainActive[nHeight]->GetBlockHash().GetHex();
    }

    uint256 hash(uint256S(strHash));

    bool fVerbose = true;
    if (request.params.size() > 1)
        fVerbose = request.params[1].get_bool();

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];

    if (fHavePruned && !(pblockindex->nStatus & BLOCK_HAVE_DATA) && pblockindex->nTx > 0)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Block not available (pruned data)");

    if (!ReadBlockFromDisk(block, pblockindex, Params().GetConsensus()))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");

    if (!fVerbose) {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << block;
        std::string strHex = HexStr(ssBlock.begin(), ssBlock.end());
        return strHex;
    }

    return blockToJSON(block, pblockindex);
}

////////////////////////////////////////////////////////////////////// // qtum

UniValue callcontract(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2)
        throw runtime_error(
            "callcontract \"address\" \"data\" ( address )\n"
            "\nArgument:\n"
            "1. \"address\"          (string, required) The account address\n"
            "2. \"data\"             (string, required) The data hex string\n"
            "3. address              (string, optional) The sender address hex string\n"
            "4. gasLimit             (string, optional) The gas limit for executing the contract\n"
        );

    LOCK(cs_main);

    std::string strAddr = request.params[0].get_str();
    std::string data = request.params[1].get_str();

    if (data.size() % 2 != 0 || !CheckHex(data))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid data (data not hex)");

    if (strAddr.size() != 40 || !CheckHex(strAddr))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Incorrect address");

    dev::Address addrAccount(strAddr);
    if (!globalState->addressInUse(addrAccount))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Address does not exist");

    dev::Address senderAddress;
    if (request.params.size() == 3) {
//        CBitcoinAddress qtumSenderAddress(request.params[2].get_str());
        CTxDestination qtumSenderAddress = DecodeDestination(request.params[2].get_str());
        const CKeyID* keyID = boost::get<CKeyID>(&qtumSenderAddress);
        if (!keyID) {
            senderAddress = dev::Address(request.params[2].get_str());
        } else {
            CKeyID keyid = *keyID;
            senderAddress = dev::Address(HexStr(valtype(keyid.begin(), keyid.end())));
        }

    }
    uint64_t gasLimit = 0;
    if (request.params.size() == 4) {
        gasLimit = request.params[3].get_int();
    }


    std::vector<ResultExecute> execResults = CallContract(addrAccount, ParseHex(data), senderAddress, gasLimit);

    if (fRecordLogOpcodes) {
        writeVMlog(execResults);
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("address", strAddr));
    result.push_back(Pair("executionResult", executionResultToJSON(execResults[0].execRes)));
    result.push_back(Pair("transactionReceipt", transactionReceiptToJSON(execResults[0].txRec)));

    return result;
}

void assignJSON(UniValue& entry, const TransactionReceiptInfo& resExec)
{
    entry.push_back(Pair("blockHash", resExec.blockHash.GetHex()));
    entry.push_back(Pair("blockNumber", uint64_t(resExec.blockNumber)));
    entry.push_back(Pair("transactionHash", resExec.transactionHash.GetHex()));
    entry.push_back(
        Pair("transactionIndex", uint64_t(resExec.transactionIndex)));
    entry.push_back(Pair("from", resExec.from.hex()));
    entry.push_back(Pair("to", resExec.to.hex()));
    entry.push_back(
        Pair("cumulativeGasUsed", CAmount(resExec.cumulativeGasUsed)));
    entry.push_back(Pair("gasUsed", CAmount(resExec.gasUsed)));
    entry.push_back(Pair("contractAddress", resExec.contractAddress.hex()));
    std::stringstream ss;
    ss << resExec.excepted;
    entry.push_back(Pair("excepted", ss.str()));
}

void assignJSON(UniValue& logEntry, const dev::eth::LogEntry& log,
                bool includeAddress)
{
    if (includeAddress) {
        logEntry.push_back(Pair("address", log.address.hex()));
    }

    UniValue topics(UniValue::VARR);
    for (dev::h256 hash : log.topics) {
        topics.push_back(hash.hex());
    }
    logEntry.push_back(Pair("topics", topics));
    logEntry.push_back(Pair("data", HexStr(log.data)));
}

void transactionReceiptInfoToJSON(const TransactionReceiptInfo& resExec, UniValue& entry)
{
    assignJSON(entry, resExec);

    const auto& logs = resExec.logs;
    UniValue logEntries(UniValue::VARR);
    for (const auto& log : logs) {
        UniValue logEntry(UniValue::VOBJ);
        assignJSON(logEntry, log, true);
        logEntries.push_back(logEntry);
    }
    entry.push_back(Pair("log", logEntries));
}

size_t parseUInt(const UniValue& val, size_t defaultVal)
{
    if (val.isNull()) {
        return defaultVal;
    } else {
        int n = val.get_int();
        if (n < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMS, "Expects unsigned integer");
        }

        return n;
    }
}

int parseBlockHeight(const UniValue& val)
{
    if (val.isStr()) {
        auto blockKey = val.get_str();

        if (blockKey == "latest") {
            return latestblock.height;
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMS, "invalid block number");
        }
    }

    if (val.isNum()) {
        int blockHeight = val.get_int();

        if (blockHeight < 0) {
            return latestblock.height;
        }

        return blockHeight;
    }

    throw JSONRPCError(RPC_INVALID_PARAMS, "invalid block number");
}

int parseBlockHeight(const UniValue& val, int defaultVal)
{
    if (val.isNull()) {
        return defaultVal;
    } else {
        return parseBlockHeight(val);
    }
}

dev::h160 parseParamH160(const UniValue& val)
{
    if (!val.isStr()) {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid hex 160");
    }

    auto addrStr = val.get_str();

    if (addrStr.length() != 40 || !CheckHex(addrStr)) {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid hex 160 string");
    }
    return dev::h160(addrStr);
}

void parseParam(const UniValue& val, std::vector<dev::h160>& h160s)
{
    if (val.isNull()) {
        return;
    }

    // Treat a string as an array of length 1
    if (val.isStr()) {
        h160s.push_back(parseParamH160(val.get_str()));
        return;
    }

    if (!val.isArray()) {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Expect an array of hex 160 strings");
    }

    auto vals = val.getValues();
    h160s.resize(vals.size());

    std::transform(vals.begin(), vals.end(), h160s.begin(), [](UniValue val) -> dev::h160 {
        return parseParamH160(val);
    });
}

void parseParam(const UniValue& val, std::set<dev::h160>& h160s)
{
    std::vector<dev::h160> v;
    parseParam(val, v);
    h160s.insert(v.begin(), v.end());
}

void parseParam(const UniValue& val, std::vector<boost::optional<dev::h256>>& h256s)
{
    if (val.isNull()) {
        return;
    }

    if (!val.isArray()) {
        throw JSONRPCError(RPC_INVALID_PARAMS, "Expect an array of hex 256 strings");
    }

    auto vals = val.getValues();
    h256s.resize(vals.size());

    std::transform(vals.begin(), vals.end(), h256s.begin(), [](UniValue val) -> boost::optional<dev::h256> {
        if (val.isNull())
        {
            return boost::optional<dev::h256>();
        }

        if (!val.isStr())
        {
            throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid hex 256 string");
        }

        auto addrStr = val.get_str();

        if (addrStr.length() != 64 || !CheckHex(addrStr))
        {
            throw JSONRPCError(RPC_INVALID_PARAMS, "Invalid hex 256 string");
        }

        return boost::optional<dev::h256>(dev::h256(addrStr));
    });
}

class WaitForLogsParams
{
public:
    int fromBlock;
    int toBlock;

    int minconf;

    std::set<dev::h160> addresses;
    std::vector<boost::optional<dev::h256>> topics;

    // bool wait;

    WaitForLogsParams(const UniValue& params)
    {
        std::unique_lock<std::mutex> lock(cs_blockchange);

        fromBlock = parseBlockHeight(params[0], latestblock.height + 1);
        toBlock = parseBlockHeight(params[1], -1);

        parseFilter(params[2]);
        minconf = parseUInt(params[3], 6);
    }

private:

    void parseFilter(const UniValue& val)
    {
        if (val.isNull()) {
            return;
        }

        parseParam(val["addresses"], addresses);
        parseParam(val["topics"], topics);
    }
};

UniValue waitforlogs(const JSONRPCRequest& request_)
{
    // this is a long poll function. force cast to non const pointer
    JSONRPCRequest& request = (JSONRPCRequest&) request_;

    if (request.fHelp) {
        throw runtime_error(
            "waitforlogs (fromBlock) (toBlock) (filter) (minconf)\n"
            "requires -logevents to be enabled\n"
            "\nWaits for a new logs and return matching log entries. When the call returns, it also specifies the next block number to start waiting for new logs.\n"
            "By calling waitforlogs repeatedly using the returned `nextBlock` number, a client can receive a stream of up-to-date log entires.\n"
            "\nThis call is different from the similarly named `waitforlogs`. This call returns individual matching log entries, `searchlogs` returns a transaction receipt if one of the log entries of that transaction matches the filter conditions.\n"
            "\nArguments:\n"
            "1. fromBlock (int | \"latest\", optional, default=null) The block number to start looking for logs. ()\n"
            "2. toBlock   (int | \"latest\", optional, default=null) The block number to stop looking for logs. If null, will wait indefinitely into the future.\n"
            "3. filter    ({ addresses?: Hex160String[], topics?: Hex256String[] }, optional default={}) Filter conditions for logs. Addresses and topics are specified as array of hexadecimal strings\n"
            "4. minconf   (uint, optional, default=6) Minimal number of confirmations before a log is returned\n"
            "\nResult:\n"
            "An object with the following properties:\n"
            "1. logs (LogEntry[]) Array of matchiing log entries. This may be empty if `filter` removed all entries."
            "2. count (int) How many log entries are returned."
            "3. nextBlock (int) To wait for new log entries haven't seen before, use this number as `fromBlock`"
            "\nUsage:\n"
            "`waitforlogs` waits for new logs, starting from the tip of the chain.\n"
            "`waitforlogs 600` waits for new logs, but starting from block 600. If there are logs available, this call will return immediately.\n"
            "`waitforlogs 600 700` waits for new logs, but only up to 700th block\n"
            "`waitforlogs null null` this is equivalent to `waitforlogs`, using default parameter values\n"
            "`waitforlogs null null` { \"addresses\": [ \"ff0011...\" ], \"topics\": [ \"c0fefe\"] }` waits for logs in the future matching the specified conditions\n"
            "\nSample Output:\n"
            "{\n  \"entries\": [\n    {\n      \"blockHash\": \"56d5f1f5ec239ef9c822d9ed600fe9aa63727071770ac7c0eabfc903bf7316d4\",\n      \"blockNumber\": 3286,\n      \"transactionHash\": \"00aa0f041ce333bc3a855b2cba03c41427cda04f0334d7f6cb0acad62f338ddc\",\n      \"transactionIndex\": 2,\n      \"from\": \"3f6866e2b59121ada1ddfc8edc84a92d9655675f\",\n      \"to\": \"8e1ee0b38b719abe8fa984c986eabb5bb5071b6b\",\n      \"cumulativeGasUsed\": 23709,\n      \"gasUsed\": 23709,\n      \"contractAddress\": \"8e1ee0b38b719abe8fa984c986eabb5bb5071b6b\",\n      \"topics\": [\n        \"f0e1159fa6dc12bb31e0098b7a1270c2bd50e760522991c6f0119160028d9916\",\n        \"0000000000000000000000000000000000000000000000000000000000000002\"\n      ],\n      \"data\": \"00000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000000000000000000000003\"\n    }\n  ],\n\n  \"count\": 7,\n  \"nextblock\": 801\n}\n"
        );
    }

    if (!fLogEvents)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Events indexing disabled");

    WaitForLogsParams params(request.params);

    request.PollStart();

    std::vector<std::vector < uint256>> hashesToBlock;

    int curheight = 0;

    auto& addresses = params.addresses;
    auto& filterTopics = params.topics;

    while (curheight == 0) {
        {
            LOCK(cs_main);
            curheight = pblocktree->ReadHeightIndex(params.fromBlock, params.toBlock, params.minconf,
                                                    hashesToBlock, addresses);
        }

        // if curheight >= fromBlock. Blockchain extended with new log entries. Return next block height to client.
        //    nextBlock = curheight + 1
        // if curheight == 0. No log entry found in index. Wait for new block then try again.
        //    nextBlock = fromBlock
        // if curheight == -1. Incorrect parameters has entered.
        //
        // if curheight advanced, but all filtered out, API should return empty array, but advancing the cursor anyway.

        if (curheight > 0) {
            break;
        }

        if (curheight == -1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Incorrect params");
        }

        // wait for a new block to arrive
        {
            while (true) {
                std::unique_lock<std::mutex> lock(cs_blockchange);
                auto blockHeight = latestblock.height;

                request.PollPing();

                cond_blockchange.wait_for(lock, std::chrono::milliseconds(1000));
                if (latestblock.height > blockHeight) {
                    break;
                }

                // TODO: maybe just merge `IsRPCRunning` this into PollAlive
                if (!request.PollAlive() || !IsRPCRunning()) {
                    LogPrintf("waitforlogs client disconnected\n");
                    return NullUniValue;
                }
            }
        }
    }

    LOCK(cs_main);

    UniValue jsonLogs(UniValue::VARR);

    for (const auto& txHashes : hashesToBlock) {
        for (const auto& txHash : txHashes) {
            std::vector<TransactionReceiptInfo> receipts = pstorageresult->getResult(
                        uintToh256(txHash));

            for (const auto& receipt : receipts) {
                for (const auto& log : receipt.logs) {

                    bool includeLog = true;

                    if (!filterTopics.empty()) {
                        for (size_t i = 0; i < filterTopics.size(); i++) {
                            auto filterTopic = filterTopics[i];

                            if (!filterTopic) {
                                continue;
                            }

                            auto filterTopicContent = filterTopic.get();
                            auto topicContent = log.topics[i];

                            if (topicContent != filterTopicContent) {
                                includeLog = false;
                                break;
                            }
                        }
                    }


                    if (!includeLog) {
                        continue;
                    }

                    UniValue jsonLog(UniValue::VOBJ);

                    assignJSON(jsonLog, receipt);
                    assignJSON(jsonLog, log, false);

                    jsonLogs.push_back(jsonLog);
                }
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("entries", jsonLogs));
    result.push_back(Pair("count", (int) jsonLogs.size()));
    result.push_back(Pair("nextblock", curheight + 1));

    return result;
}

class SearchLogsParams
{
public:
    size_t fromBlock;
    size_t toBlock;
    size_t minconf;

    std::set<dev::h160> addresses;
    std::vector<boost::optional<dev::h256>> topics;

    SearchLogsParams(const UniValue& params)
    {
        std::unique_lock<std::mutex> lock(cs_blockchange);

        setFromBlock(params[0]);
        setToBlock(params[1]);

        parseParam(params[2]["addresses"], addresses);
        parseParam(params[3]["topics"], topics);

        minconf = parseUInt(params[4], 0);
    }

private:

    void setFromBlock(const UniValue& val)
    {
        if (!val.isNull()) {
            fromBlock = parseBlockHeight(val);
        } else {
            fromBlock = latestblock.height;
        }
    }

    void setToBlock(const UniValue& val)
    {
        if (!val.isNull()) {
            toBlock = parseBlockHeight(val);
        } else {
            toBlock = latestblock.height;
        }
    }

};

UniValue searchlogs(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2)
        throw runtime_error(
            "searchlogs <fromBlock> <toBlock> (address) (topics)\n"
            "requires -logevents to be enabled"
            "\nArgument:\n"
            "1. \"fromBlock\"        (numeric, required) The number of the earliest block (latest may be given to mean the most recent block).\n"
            "2. \"toBlock\"          (string, required) The number of the latest block (-1 may be given to mean the most recent block).\n"
            "3. \"address\"          (string, optional) An address or a list of addresses to only get logs from particular account(s).\n"
            "4. \"topics\"           (string, optional) An array of values from which at least one must appear in the log entries. The order is important, if you want to leave topics out use null, e.g. [\"null\", \"0x00...\"]. \n"
            "5. \"minconf\"          (uint, optional, default=0) Minimal number of confirmations before a log is returned\n"
            "\nExamples:\n"
            + HelpExampleCli("searchlogs", "0 100 '{\"addresses\": [\"12ae42729af478ca92c8c66773a3e32115717be4\"]}' '{\"topics\": [\"null\",\"b436c2bf863ccd7b8f63171201efd4792066b4ce8e543dde9c3e9e9ab98e216c\"]}'")
            + HelpExampleRpc("searchlogs", "0 100 {\"addresses\": [\"12ae42729af478ca92c8c66773a3e32115717be4\"]} {\"topics\": [\"null\",\"b436c2bf863ccd7b8f63171201efd4792066b4ce8e543dde9c3e9e9ab98e216c\"]}")
        );

    if (!fLogEvents)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Events indexing disabled");

    int curheight = 0;

    LOCK(cs_main);

    SearchLogsParams params(request.params);

    std::vector<std::vector < uint256>> hashesToBlock;

    curheight = pblocktree->ReadHeightIndex(params.fromBlock, params.toBlock, params.minconf, hashesToBlock, params.addresses);

    if (curheight == -1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Incorrect params");
    }

    UniValue result(UniValue::VARR);

    auto topics = params.topics;

    for (const auto& hashesTx : hashesToBlock) {
        for (const auto& e : hashesTx) {
            std::vector<TransactionReceiptInfo> receipts = pstorageresult->getResult(uintToh256(e));

            for (const auto& receipt : receipts) {
                if (receipt.logs.empty()) {
                    continue;
                }

                if (!topics.empty()) {
                    for (size_t i = 0; i < topics.size(); i++) {
                        const auto& tc = topics[i];

                        if (!tc) {
                            continue;
                        }

                        for (const auto& log : receipt.logs) {
                            auto filterTopicContent = tc.get();

                            if (i >= log.topics.size()) {
                                continue;
                            }

                            if (filterTopicContent == log.topics[i]) {
                                goto push;
                            }
                        }
                    }

                    // Skip the log if none of the topics are matched
                    continue;
                }

push:

                UniValue tri(UniValue::VOBJ);
                transactionReceiptInfoToJSON(receipt, tri);
                result.push_back(tri);
            }
        }
    }

    return result;
}

UniValue gettransactionreceipt(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1)
        throw runtime_error(
            "gettransactionreceipt \"hash\"\n"
            "requires -logevents to be enabled"
            "\nArgument:\n"
            "1. \"hash\"          (string, required) The transaction hash\n"
        );

    if (!fLogEvents)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Events indexing disabled");

    LOCK(cs_main);

    std::string hashTemp = request.params[0].get_str();
    if (hashTemp.size() != 64) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Incorrect hash");
    }

    uint256 hash(uint256S(hashTemp));

    std::vector<TransactionReceiptInfo> transactionReceiptInfo = pstorageresult->getResult(uintToh256(hash));

    UniValue result(UniValue::VARR);
    for (TransactionReceiptInfo& t : transactionReceiptInfo) {
        UniValue tri(UniValue::VOBJ);
        transactionReceiptInfoToJSON(t, tri);
        result.push_back(tri);
    }
    return result;
}
//////////////////////////////////////////////////////////////////////

UniValue listcontracts(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw runtime_error(
            "listcontracts (start maxDisplay)\n"
            "\nArgument:\n"
            "1. start     (numeric or string, optional) The starting account index, default 1\n"
            "2. maxDisplay       (numeric or string, optional) Max accounts to list, default 20\n"
        );

    LOCK(cs_main);

    int start = 1;
    if (request.params.size() > 0) {
        start = request.params[0].get_int();
        if (start <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid start, min=1");
    }

    int maxDisplay = 20;
    if (request.params.size() > 1) {
        maxDisplay = request.params[1].get_int();
        if (maxDisplay <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid maxDisplay");
    }

    UniValue result(UniValue::VOBJ);

    auto map = globalState->addresses();
    int contractsCount = (int) map.size();

    if (contractsCount > 0 && start > contractsCount)
        throw JSONRPCError(RPC_TYPE_ERROR, "start greater than max index " + itostr(contractsCount));

    int itStartPos = std::min(start - 1, contractsCount);
    int i = 0;
    for (auto it = std::next(map.begin(), itStartPos); it != map.end(); it++) {
        result.push_back(Pair(it->first.hex(), ValueFromAmount(CAmount(globalState->balance(it->first)))));
        i++;
        if (i == maxDisplay)break;
    }

    return result;
}

static void ApplyStats(CCoinsStats& stats, CHashWriter& ss, const uint256& hash, const std::map<uint32_t, Coin>& outputs)
{
    assert(!outputs.empty());
    ss << hash;
    ss << VARINT(outputs.begin()->second.nHeight * 2 + outputs.begin()->second.fCoinBase);
    stats.nTransactions++;
    for (const auto output : outputs) {
        ss << VARINT(output.first + 1);
        ss << *(const CScriptBase*) (&output.second.out.scriptPubKey);
        ss << VARINT(output.second.out.nValue);
        stats.nTransactionOutputs++;
        stats.nTotalAmount += output.second.out.nValue;
    }
    ss << VARINT(0);
}

//! Calculate statistics about the unspent transaction output set

static bool GetUTXOStats(CCoinsView* view, CCoinsStats& stats)
{
    boost::scoped_ptr<CCoinsViewCursor> pcursor(view->Cursor());

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    stats.hashBlock = pcursor->GetBestBlock();
    {
        LOCK(cs_main);
        stats.nHeight = mapBlockIndex.find(stats.hashBlock)->second->nHeight;
    }
    ss << stats.hashBlock;
    uint256 prevkey;
    std::map<uint32_t, Coin> outputs;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        COutPoint key;
        Coin coin;
        if (pcursor->GetKey(key) && pcursor->GetValue(coin)) {
            if (!outputs.empty() && key.hash != prevkey) {
                ApplyStats(stats, ss, prevkey, outputs);
                outputs.clear();
            }
            prevkey = key.hash;
            outputs[key.n] = std::move(coin);
        } else {
            return error("%s: unable to read value", __func__);
        }
        pcursor->Next();
    }
    if (!outputs.empty()) {
        ApplyStats(stats, ss, prevkey, outputs);
    }
    stats.hashSerialized = ss.GetHash();
    stats.nDiskSize = view->EstimateSize();
    return true;
}

UniValue gettxoutsetinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "gettxoutsetinfo\n"
            "\nReturns statistics about the unspent transaction output set.\n"
            "Note this call may take some time.\n"
            "\nResult:\n"
            "{\n"
            "  \"height\":n,     (numeric) The current block height (index)\n"
            "  \"bestblock\": \"hex\",   (string) the best block hash hex\n"
            "  \"transactions\": n,      (numeric) The number of transactions\n"
            "  \"txouts\": n,            (numeric) The number of output transactions\n"
            "  \"bytes_serialized\": n,  (numeric) The serialized size\n"
            "  \"hash_serialized\": \"hash\",   (string) The serialized hash\n"
            "  \"total_amount\": x.xxx          (numeric) The total amount\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("gettxoutsetinfo", "")
            + HelpExampleRpc("gettxoutsetinfo", "")
        );

    UniValue ret(UniValue::VOBJ);

    CCoinsStats stats;
    FlushStateToDisk();
    if (GetUTXOStats(pcoinsdbview, stats)) {
        ret.push_back(Pair("height", (int64_t) stats.nHeight));
        ret.push_back(Pair("bestblock", stats.hashBlock.GetHex()));
        ret.push_back(Pair("transactions", (int64_t) stats.nTransactions));
        ret.push_back(Pair("txouts", (int64_t) stats.nTransactionOutputs));
        ret.push_back(Pair("hash_serialized_2", stats.hashSerialized.GetHex()));
        ret.push_back(Pair("disk_size", stats.nDiskSize));
        ret.push_back(Pair("total_amount", ValueFromAmount(stats.nTotalAmount)));
    }
    return ret;
}

UniValue gettxout(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw runtime_error(
            "gettxout \"txid\" n ( include_mempool )\n"
            "\nReturns details about an unspent transaction output.\n"
            "\nArguments:\n"
            "1. \"txid\"       (string, required) The transaction id\n"
            "2. n              (numeric, required) vout number\n"
            "3. include_mempool  (boolean, optional) Whether to include the mempool\n"
            "\nResult:\n"
            "{\n"
            "  \"bestblock\" : \"hash\",    (string) the block hash\n"
            "  \"confirmations\" : n,       (numeric) The number of confirmations\n"
            "  \"value\" : x.xxx,           (numeric) The transaction value in " + CURRENCY_UNIT + "\n"
            "  \"scriptPubKey\" : {         (json object)\n"
            "     \"asm\" : \"code\",       (string) \n"
            "     \"hex\" : \"hex\",        (string) \n"
            "     \"reqSigs\" : n,          (numeric) Number of required signatures\n"
            "     \"type\" : \"pubkeyhash\", (string) The type, eg pubkeyhash\n"
            "     \"addresses\" : [          (array of string) array of bitcoin addresses\n"
            "        \"address\"     (string) bitcoin address\n"
            "        ,...\n"
            "     ]\n"
            "  },\n"
            "  \"version\" : n,            (numeric) The version\n"
            "  \"coinbase\" : true|false   (boolean) Coinbase or not\n"
            "}\n"

            "\nExamples:\n"
            "\nGet unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nView the details\n"
            + HelpExampleCli("gettxout", "\"txid\" 1") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("gettxout", "\"txid\", 1")
        );

    LOCK(cs_main);

    UniValue ret(UniValue::VOBJ);

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));
    int n = request.params[1].get_int();
    COutPoint out(hash, n);
    bool fMempool = true;
    if (request.params.size() > 2)
        fMempool = request.params[2].get_bool();

    Coin coin;
    if (fMempool) {
        LOCK(mempool.cs);
        CCoinsViewMemPool view(pcoinsTip, mempool);
        if (!view.GetCoin(out, coin) || mempool.isSpent(out)) { // TODO: filtering spent coins should be done by the CCoinsViewMemPool
            return NullUniValue;
        }
    } else {
        if (!pcoinsTip->GetCoin(out, coin)) {
            return NullUniValue;
        }
    }

    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    CBlockIndex* pindex = it->second;
    ret.push_back(Pair("bestblock", pindex->GetBlockHash().GetHex()));
    if (coin.nHeight == MEMPOOL_HEIGHT) {
        ret.push_back(Pair("confirmations", 0));
    } else {
        ret.push_back(Pair("confirmations", (int64_t) (pindex->nHeight - coin.nHeight + 1)));
    }
    ret.push_back(Pair("value", ValueFromAmount(coin.out.nValue)));
    UniValue o(UniValue::VOBJ);
    ScriptPubKeyToJSON(coin.out.scriptPubKey, o, true);
    ret.push_back(Pair("scriptPubKey", o));
    ret.push_back(Pair("coinbase", (bool)coin.fCoinBase));

    return ret;
}

UniValue verifychain(const JSONRPCRequest& request)
{
    int nCheckLevel = GetArg("-checklevel", DEFAULT_CHECKLEVEL);
    int nCheckDepth = GetArg("-checkblocks", DEFAULT_CHECKBLOCKS);
    if (request.fHelp || request.params.size() > 2)
        throw runtime_error(
            "verifychain ( checklevel nblocks )\n"
            "\nVerifies blockchain database.\n"
            "\nArguments:\n"
            "1. checklevel   (numeric, optional, 0-4, default=" + strprintf("%d", nCheckLevel) + ") How thorough the block verification is.\n"
            "2. nblocks      (numeric, optional, default=" + strprintf("%d", nCheckDepth) + ", 0=all) The number of blocks to check.\n"
            "\nResult:\n"
            "true|false       (boolean) Verified or not\n"
            "\nExamples:\n"
            + HelpExampleCli("verifychain", "")
            + HelpExampleRpc("verifychain", "")
        );

    LOCK(cs_main);

    if (request.params.size() > 0)
        nCheckLevel = request.params[0].get_int();
    if (request.params.size() > 1)
        nCheckDepth = request.params[1].get_int();

    return CVerifyDB().VerifyDB(Params(), pcoinsTip, pclueTip, nCheckLevel, nCheckDepth);
}

/** Implementation of IsSuperMajority with better feedback */
static UniValue SoftForkMajorityDesc(int minVersion, CBlockIndex* pindex, int nRequired, const Consensus::Params& consensusParams)
{
    int nFound = 0;
    CBlockIndex* pstart = pindex;
    for (int i = 0; i < consensusParams.nMajorityWindow && pstart != NULL; i++) {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }

    UniValue rv(UniValue::VOBJ);
    rv.push_back(Pair("status", nFound >= nRequired));
    rv.push_back(Pair("found", nFound));
    rv.push_back(Pair("required", nRequired));
    rv.push_back(Pair("window", consensusParams.nMajorityWindow));
    return rv;
}

static UniValue SoftForkDesc(const std::string& name, int version, CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    UniValue rv(UniValue::VOBJ);
    rv.push_back(Pair("id", name));
    rv.push_back(Pair("version", version));
    rv.push_back(Pair("enforce", SoftForkMajorityDesc(version, pindex, consensusParams.nMajorityEnforceBlockUpgrade, consensusParams)));
    rv.push_back(Pair("reject", SoftForkMajorityDesc(version, pindex, consensusParams.nMajorityRejectBlockOutdated, consensusParams)));
    return rv;
}

UniValue getblockchaininfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getblockchaininfo\n"
            "Returns an object containing various state info regarding block chain processing.\n"
            "\nResult:\n"
            "{\n"
            "  \"chain\": \"xxxx\",        (string) current network name as defined in BIP70 (main, test, regtest)\n"
            "  \"blocks\": xxxxxx,         (numeric) the current number of blocks processed in the server\n"
            "  \"headers\": xxxxxx,        (numeric) the current number of headers we have validated\n"
            "  \"bestblockhash\": \"...\", (string) the hash of the currently best block\n"
            "  \"difficulty\": xxxxxx,     (numeric) the current difficulty\n"
            "  \"verificationprogress\": xxxx, (numeric) estimate of verification progress [0..1]\n"
            "  \"chainwork\": \"xxxx\"     (string) total amount of work in active chain, in hexadecimal\n"
            "  \"commitments\": xxxxxx,    (numeric) the current number of note commitments in the commitment tree\n"
            "  \"softforks\": [            (array) status of softforks in progress\n"
            "     {\n"
            "        \"id\": \"xxxx\",        (string) name of softfork\n"
            "        \"version\": xx,         (numeric) block version\n"
            "        \"enforce\": {           (object) progress toward enforcing the softfork rules for new-version blocks\n"
            "           \"status\": xx,       (boolean) true if threshold reached\n"
            "           \"found\": xx,        (numeric) number of blocks with the new version found\n"
            "           \"required\": xx,     (numeric) number of blocks required to trigger\n"
            "           \"window\": xx,       (numeric) maximum size of examined window of recent blocks\n"
            "        },\n"
            "        \"reject\": { ... }      (object) progress toward rejecting pre-softfork blocks (same fields as \"enforce\")\n"
            "     }, ...\n"
            "  ]\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getblockchaininfo", "")
            + HelpExampleRpc("getblockchaininfo", "")
        );

    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("chain", Params().NetworkIDString()));
    obj.push_back(Pair("blocks", (int) chainActive.Height()));
    obj.push_back(Pair("headers", pindexBestHeader ? pindexBestHeader->nHeight : -1));
    obj.push_back(Pair("bestblockhash", chainActive.Tip()->GetBlockHash().GetHex()));
    obj.push_back(Pair("difficulty", (double) GetNetworkDifficulty()));
    obj.push_back(Pair("verificationprogress", Checkpoints::GuessVerificationProgress(Params().Checkpoints(), chainActive.Tip())));
    obj.push_back(Pair("chainwork", chainActive.Tip()->nChainWork.GetHex()));
    obj.push_back(Pair("pruned", fPruneMode));


    CBlockIndex* tip = chainActive.Tip();
    UniValue valuePools(UniValue::VARR);
    valuePools.push_back(ValuePoolDesc("sapling", tip->nChainSaplingValue, boost::none));
    obj.push_back(Pair("valuePools",            valuePools));

    const Consensus::Params& consensusParams = Params().GetConsensus();
    UniValue softforks(UniValue::VARR);
    softforks.push_back(SoftForkDesc("bip34", 2, tip, consensusParams));
    softforks.push_back(SoftForkDesc("bip66", 3, tip, consensusParams));
    softforks.push_back(SoftForkDesc("bip65", 4, tip, consensusParams));
    obj.push_back(Pair("softforks", softforks));

    if (fPruneMode) {
        CBlockIndex* block = chainActive.Tip();
        while (block && block->pprev && (block->pprev->nStatus & BLOCK_HAVE_DATA))
            block = block->pprev;

        obj.push_back(Pair("pruneheight", block->nHeight));
    }
    return obj;
}

/** Comparison function for sorting the getchaintips heads.  */
struct CompareBlocksByHeight {

    bool operator()(const CBlockIndex* a, const CBlockIndex* b) const
    {
        /* Make sure that unequal blocks with the same height do not compare
           equal. Use the pointers themselves to make a distinction. */

        if (a->nHeight != b->nHeight)
            return (a->nHeight > b->nHeight);

        return a < b;
    }
};

UniValue getchaintips(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getchaintips\n"
            "Return information about all known tips in the block tree,"
            " including the main chain as well as orphaned branches.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"height\": xxxx,         (numeric) height of the chain tip\n"
            "    \"hash\": \"xxxx\",         (string) block hash of the tip\n"
            "    \"branchlen\": 0          (numeric) zero for main chain\n"
            "    \"status\": \"active\"      (string) \"active\" for the main chain\n"
            "  },\n"
            "  {\n"
            "    \"height\": xxxx,\n"
            "    \"hash\": \"xxxx\",\n"
            "    \"branchlen\": 1          (numeric) length of branch connecting the tip to the main chain\n"
            "    \"status\": \"xxxx\"        (string) status of the chain (active, valid-fork, valid-headers, headers-only, invalid)\n"
            "  }\n"
            "]\n"
            "Possible values for status:\n"
            "1.  \"invalid\"               This branch contains at least one invalid block\n"
            "2.  \"headers-only\"          Not all blocks for this branch are available, but the headers are valid\n"
            "3.  \"valid-headers\"         All blocks are available for this branch, but they were never fully validated\n"
            "4.  \"valid-fork\"            This branch is not part of the active chain, but is fully validated\n"
            "5.  \"active\"                This is the tip of the active main chain, which is certainly valid\n"
            "\nExamples:\n"
            + HelpExampleCli("getchaintips", "")
            + HelpExampleRpc("getchaintips", "")
        );

    LOCK(cs_main);

    /* Build up a list of chain tips.  We start with the list of all
       known blocks, and successively remove blocks that appear as pprev
       of another block.  */
    std::set<const CBlockIndex*, CompareBlocksByHeight> setTips;
    BOOST_FOREACH(const PAIRTYPE(const uint256, CBlockIndex*) & item, mapBlockIndex)
    setTips.insert(item.second);

    BOOST_FOREACH(const PAIRTYPE(const uint256, CBlockIndex*) & item, mapBlockIndex) {
        const CBlockIndex* pprev = item.second->pprev;
        if (pprev)
            setTips.erase(pprev);
    }

    // Always report the currently active tip.
    setTips.insert(chainActive.Tip());

    /* Construct the output array.  */
    UniValue res(UniValue::VARR);

    BOOST_FOREACH(const CBlockIndex * block, setTips) {
        UniValue obj(UniValue::VOBJ);
        obj.push_back(Pair("height", block->nHeight));
        obj.push_back(Pair("hash", block->phashBlock->GetHex()));

        const int branchLen = block->nHeight - chainActive.FindFork(block)->nHeight;
        obj.push_back(Pair("branchlen", branchLen));

        string status;
        if (chainActive.Contains(block)) {
            // This block is part of the currently active chain.
            status = "active";
        } else if (block->nStatus & BLOCK_FAILED_MASK) {
            // This block or one of its ancestors is invalid.
            status = "invalid";
        } else if (block->nChainTx == 0) {
            // This block cannot be connected because full block data for it or one of its parents is missing.
            status = "headers-only";
        } else if (block->IsValid(BLOCK_VALID_SCRIPTS)) {
            // This block is fully validated, but no longer part of the active chain. It was probably the active block once, but was reorganized.
            status = "valid-fork";
        } else if (block->IsValid(BLOCK_VALID_TREE)) {
            // The headers for this block are valid, but it has not been validated. It was probably never part of the most-work chain.
            status = "valid-headers";
        } else {
            // No clue.
            status = "unknown";
        }
        obj.push_back(Pair("status", status));

        res.push_back(obj);
    }

    return res;
}

UniValue mempoolInfoToJSON()
{
    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("size", (int64_t) mempool.size()));
    ret.push_back(Pair("bytes", (int64_t) mempool.GetTotalTxSize()));
    ret.push_back(Pair("usage", (int64_t) mempool.DynamicMemoryUsage()));

    return ret;
}

UniValue getmempoolinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
            "getmempoolinfo\n"
            "\nReturns details on the active state of the TX memory pool.\n"
            "\nResult:\n"
            "{\n"
            "  \"size\": xxxxx,               (numeric) Current tx count\n"
            "  \"bytes\": xxxxx,              (numeric) Sum of all virtual transaction sizes as defined in BIP 141. Differs from actual serialized size because witness data is discounted\n"
            "  \"usage\": xxxxx              (numeric) Total memory usage for the mempool\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getmempoolinfo", "")
            + HelpExampleRpc("getmempoolinfo", "")
        );

    return mempoolInfoToJSON();
}

UniValue invalidateblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "invalidateblock \"blockhash\"\n"
            "\nPermanently marks a block as invalid, as if it violated a consensus rule.\n"
            "\nArguments:\n"
            "1. \"blockhash\"   (string, required) the hash of the block to mark as invalid\n"
            "\nResult:\n"
            "\nExamples:\n"
            + HelpExampleCli("invalidateblock", "\"blockhash\"")
            + HelpExampleRpc("invalidateblock", "\"blockhash\"")
        );

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));
    CValidationState state;

    {
        LOCK(cs_main);
        if (mapBlockIndex.count(hash) == 0)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

        CBlockIndex* pblockindex = mapBlockIndex[hash];
        InvalidateBlock(state, Params().GetConsensus(), pblockindex);
    }

    if (state.IsValid()) {
        ActivateBestChain(state, Params());
    }

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.GetRejectReason());
    }

    return NullUniValue;
}

UniValue reconsiderblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "reconsiderblock \"blockhash\"\n"
            "\nRemoves invalidity status of a block and its descendants, reconsider them for activation.\n"
            "This can be used to undo the effects of invalidateblock.\n"
            "\nArguments:\n"
            "1. \"blockhash\"   (string, required) the hash of the block to reconsider\n"
            "\nResult:\n"
            "\nExamples:\n"
            + HelpExampleCli("reconsiderblock", "\"blockhash\"")
            + HelpExampleRpc("reconsiderblock", "\"blockhash\"")
        );

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));
    CValidationState state;

    {
        LOCK(cs_main);
        if (mapBlockIndex.count(hash) == 0)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

        CBlockIndex* pblockindex = mapBlockIndex[hash];
        ReconsiderBlock(state, pblockindex);
    }

    if (state.IsValid()) {
        ActivateBestChain(state, Params());
    }

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.GetRejectReason());
    }

    return NullUniValue;
}

static const CRPCCommand commands[] = {
    //  category              name                      actor (function)         okSafe argNames
    //  --------------------- ------------------------  -----------------------  ------ ----------
    { "blockchain",         "getblockchaininfo",      &getblockchaininfo,      true,  {} },
    { "blockchain",         "getbestblockhash",       &getbestblockhash,       true,  {} },
    { "blockchain",         "getblockcount",          &getblockcount,          true,  {} },
    { "blockchain",         "getblock",               &getblock,               true,  {"blockhash", "verbose"} },
    { "blockchain",         "getblockhash",           &getblockhash,           true,  {"height"} },
    { "blockchain",         "getblockheader",         &getblockheader,         true,  {"blockhash", "verbose"} },
    { "blockchain",         "getchaintips",           &getchaintips,           true,  {} },
    { "blockchain",         "getdifficulty",          &getdifficulty,          true,  {} },
    { "blockchain",         "getmempoolinfo",         &getmempoolinfo,         true,  {} },
    { "blockchain",         "getrawmempool",          &getrawmempool,          true,  {"verbose"} },
    { "blockchain",         "gettxout",               &gettxout,               true,  {"txid", "n", "include_mempool"} },
    { "blockchain",         "gettxoutsetinfo",        &gettxoutsetinfo,        true,  {} },
    { "blockchain",         "verifychain",            &verifychain,            true,  {"checklevel", "nblocks"} },
#ifdef VDEBUG
    { "blockchain",         "callcontract",           &callcontract,           true,  {"address", "data"} }, // qtum

    { "blockchain",         "listcontracts",          &listcontracts,          true,  {"start", "maxDisplay"} },
    { "blockchain",         "gettransactionreceipt",  &gettransactionreceipt,  true,  {"hash"} },
    { "blockchain",         "searchlogs",             &searchlogs,             true,  {"fromBlock", "toBlock", "address", "topics"} },

    { "blockchain",         "waitforlogs",            &waitforlogs,            true,  {"fromBlock", "nblocks", "address", "topics"} },
#endif
    /* Not shown in help */
    { "hidden",             "invalidateblock",        &invalidateblock,        true,  {"blockhash"} },
    { "hidden",             "reconsiderblock",        &reconsiderblock,        true,  {"blockhash"} },

};

void RegisterBlockchainRPCCommands(CRPCTable& t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
