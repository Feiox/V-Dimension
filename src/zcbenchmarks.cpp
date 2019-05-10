#include <cstdio>
#include <future>
#include <map>
#include <thread>
#include <unistd.h>
#include <boost/filesystem.hpp>

#include "coins.h"
#include "util.h"
#include "init.h"
#include "primitives/transaction.h"
#include "policy/policy.h"
#include "base58.h"
#include "crypto/equihash.h"
#include "chain.h"
#include "chainparams.h"
#include "consensus/validation.h"
#include "validation.h"
#include "miner.h"
#include "pow.h"
#include "rpc/server.h"
#include "script/sign.h"
#include "sodium.h"
#include "streams.h"
#include "txdb.h"
#include "wallet/wallet.h"
#include "net_processing.h"

#include "zcbenchmarks.h"

#include "vds/Vds.h"
#include "vds/IncrementalMerkleTree.hpp"

using namespace libzcash;

void pre_wallet_load()
{
    LogPrintf("%s: In progress...\n", __func__);
    if (ShutdownRequested())
        throw new std::runtime_error("The node is shutting down");

    if (pwalletMain)
        pwalletMain->Flush(false);

    UnregisterNodeSignals(GetNodeSignals());
    if (pwalletMain)
        pwalletMain->Flush(true);

    UnregisterValidationInterface(pwalletMain);
    delete pwalletMain;
    pwalletMain = NULL;
    bitdb.Reset();
    RegisterNodeSignals(GetNodeSignals());
    LogPrintf("%s: done\n", __func__);
}

void post_wallet_load()
{
    RegisterValidationInterface(pwalletMain);

}

void timer_start(timeval& tv_start)
{
    gettimeofday(&tv_start, 0);
}

double timer_stop(timeval& tv_start)
{
    double elapsed;
    struct timeval tv_end;
    gettimeofday(&tv_end, 0);
    elapsed = double(tv_end.tv_sec - tv_start.tv_sec) +
              (tv_end.tv_usec - tv_start.tv_usec) / double(1000000);
    return elapsed;
}

double benchmark_sleep()
{
    struct timeval tv_start;
    timer_start(tv_start);
    sleep(1);
    return timer_stop(tv_start);
}

#ifdef ENABLE_MINING
double benchmark_solve_equihash()
{
    CBlock pblock;
    CEquihashInput I{pblock};
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << I;

    unsigned int n = Params(CBaseChainParams::MAIN).EquihashN();
    unsigned int k = Params(CBaseChainParams::MAIN).EquihashK();
    crypto_generichash_blake2b_state eh_state;
    EhInitialiseState(n, k, eh_state);
    crypto_generichash_blake2b_update(&eh_state, (unsigned char*)&ss[0], ss.size());

    uint256 nonce;
    randombytes_buf(nonce.begin(), 32);
    crypto_generichash_blake2b_update(&eh_state,
                                      nonce.begin(),
                                      nonce.size());

    struct timeval tv_start;
    timer_start(tv_start);
    std::set<std::vector<unsigned int>> solns;
    EhOptimisedSolveUncancellable(n, k, eh_state,
    [](std::vector<unsigned char> soln) {
        return false;
    });
    return timer_stop(tv_start);
}

std::vector<double> benchmark_solve_equihash_threaded(int nThreads)
{
    std::vector<double> ret;
    std::vector<std::future<double>> tasks;
    std::vector<std::thread> threads;
    for (int i = 0; i < nThreads; i++) {
        std::packaged_task<double(void)> task(&benchmark_solve_equihash);
        tasks.emplace_back(task.get_future());
        threads.emplace_back(std::move(task));
    }
    std::future_status status;
    for (auto it = tasks.begin(); it != tasks.end(); it++) {
        it->wait();
        ret.push_back(it->get());
    }
    for (auto it = threads.begin(); it != threads.end(); it++) {
        it->join();
    }
    return ret;
}
#endif // ENABLE_MINING

double benchmark_verify_equihash()
{
    CChainParams params = Params(CBaseChainParams::MAIN);
    CBlock genesis = Params(CBaseChainParams::MAIN).GenesisBlock();
    CBlockHeader genesis_header = genesis.GetBlockHeader();
    struct timeval tv_start;
    timer_start(tv_start);
    CheckEquihashSolution(&genesis_header, params);
    return timer_stop(tv_start);
}

double benchmark_large_tx(size_t nInputs)
{
    // Create priv/pub key
    CKey priv;
    priv.MakeNewKey(false);
    auto pub = priv.GetPubKey();
    CBasicKeyStore tempKeystore;
    tempKeystore.AddKey(priv);

    // The "original" transaction that the spending transaction will spend
    // from.
    CMutableTransaction m_orig_tx;
    m_orig_tx.vout.resize(1);
    m_orig_tx.vout[0].nValue = 1000000;
    CScript prevPubKey = GetScriptForDestination(pub.GetID());
    m_orig_tx.vout[0].scriptPubKey = prevPubKey;

    auto orig_tx = CTransaction(m_orig_tx);

    CMutableTransaction spending_tx;
    auto input_hash = orig_tx.GetHash();
    // Add nInputs inputs
    for (size_t i = 0; i < nInputs; i++) {
        spending_tx.vin.emplace_back(input_hash, 0);
    }

    // Sign for all the inputs
    for (size_t i = 0; i < nInputs; i++) {
        SignSignature(tempKeystore, prevPubKey, spending_tx, i, SIGHASH_ALL);
    }

    // Spending tx has all its inputs signed and does not need to be mutated anymore
    CTransaction final_spending_tx(spending_tx);

    // Benchmark signature verification costs:
    struct timeval tv_start;
    timer_start(tv_start);
    PrecomputedTransactionData txdata(final_spending_tx);
    for (size_t i = 0; i < nInputs; i++) {
        ScriptError serror = SCRIPT_ERR_OK;
        assert(VerifyScript(final_spending_tx.vin[i].scriptSig,
                            prevPubKey,
                            &final_spending_tx.vin[i].scriptWitness,
                            STANDARD_SCRIPT_VERIFY_FLAGS,
                            TransactionSignatureChecker(&final_spending_tx, i, txdata),
                            &serror));
    }
    return timer_stop(tv_start);
}

// Fake the input of a given block
//class FakeCoinsViewDB : public CCoinsViewDB {
//    uint256 hash;
//    SproutMerkleTree t;

//public:
//    FakeCoinsViewDB(std::string dbName, uint256& hash) : CCoinsViewDB(dbName, 100, false, false), hash(hash) {}

//    bool GetAnchorAt(const uint256 &rt, SproutMerkleTree &tree) const {
//        if (rt == t.root()) {
//            tree = t;
//            return true;
//        }
//        return false;
//    }

//    bool GetNullifier(const uint256 &nf, ShieldedType type) const {
//        return false;
//    }

//    uint256 GetBestBlock() const {
//        return hash;
//    }

//    uint256 GetBestAnchor() const {
//        return t.root();
//    }

//    bool BatchWrite(CCoinsMap &mapCoins,
//                    const uint256 &hashBlock,
//                    const uint256 &hashAnchor,
//                    CAnchorsSproutMap &mapSproutAnchors,
//                    CNullifiersMap &mapSproutNullifiers,
//                    CNullifiersMap& mapSaplingNullifiers) {
//        return false;
//    }

//    bool GetStats(CCoinsStats &stats) const {
//        return false;
//    }
//};

double benchmark_connectblock_slow()
{
    // Test for issue 2017-05-01.a
    SelectParams(CBaseChainParams::MAIN);
    CBlock block;
    FILE* fp = fopen((GetDataDir() / "benchmark/block-107134.dat").string().c_str(), "rb");
    if (!fp) throw new std::runtime_error("Failed to open block data file");
    CAutoFile blkFile(fp, SER_DISK, CLIENT_VERSION);
    blkFile >> block;
    blkFile.fclose();

    // Fake its inputs
    auto hashPrev = uint256S("00000000159a41f468e22135942a567781c3f3dc7ad62257993eb3c69c3f95ef");
//    FakeCoinsViewDB fakeDB("benchmark/block-107134-inputs", hashPrev);
//    CCoinsViewCache view(&fakeDB);

    // Fake the chain
    CBlockIndex index(block);
    index.nHeight = 107134;
    CBlockIndex indexPrev;
    indexPrev.phashBlock = &hashPrev;
    indexPrev.nHeight = index.nHeight - 1;
    index.pprev = &indexPrev;
    mapBlockIndex.insert(std::make_pair(hashPrev, &indexPrev));

    CValidationState state;
    struct timeval tv_start;
    timer_start(tv_start);
//    assert(ConnectBlock(block, state, &index, view, true));
    auto duration = timer_stop(tv_start);

    // Undo alterations to global state
    mapBlockIndex.erase(hashPrev);
//    SelectParamsFromCommandLine(); // TODO: removed by merge dash

    return duration;
}

extern UniValue getnewaddress(const JSONRPCRequest& request); // in rpcwallet.cpp
extern UniValue sendtoaddress(const JSONRPCRequest& request);

double benchmark_sendtoaddress(CAmount amount)
{
    UniValue params(UniValue::VARR);
    JSONRPCRequest reqs;
    reqs.params = params;
    auto addr = getnewaddress(reqs);

    params.push_back(addr);
    params.push_back(ValueFromAmount(amount));

    struct timeval tv_start;
    timer_start(tv_start);
    reqs.params = params;
    auto txid = sendtoaddress(reqs);
    return timer_stop(tv_start);
}

double benchmark_loadwallet()
{
    pre_wallet_load();
    struct timeval tv_start;
    bool fFirstRunRet = true;
    timer_start(tv_start);
    pwalletMain = new CWallet("wallet.dat");
    DBErrors nLoadWalletRet = pwalletMain->LoadWallet(fFirstRunRet);
    auto res = timer_stop(tv_start);
    post_wallet_load();
    return res;
}

extern UniValue listunspent(const JSONRPCRequest& request);

double benchmark_listunspent()
{
    UniValue params(UniValue::VARR);
    JSONRPCRequest req;
    struct timeval tv_start;
    timer_start(tv_start);
    req.params = params;
    auto unspent = listunspent(req);
    return timer_stop(tv_start);
}
