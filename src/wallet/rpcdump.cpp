// Copyright (c) 2014-2019 The vds Core developers
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "rpc/server.h"
#include "init.h"
#include "key_io.h"
#include "validation.h"
#include "script/script.h"
#include "script/standard.h"
#include "sync.h"
#include "util.h"
#include "utiltime.h"
#include "wallet.h"
#include "bip38_key.h"
#include "rpc/server.h"

#include <fstream>
#include <stdint.h>

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/assign/list_of.hpp>

#include <univalue.h>

using namespace std;

void EnsureWalletIsUnlocked();
bool EnsureWalletIsAvailable(bool avoidException);

UniValue dumpwallet_impl(const JSONRPCRequest& request, bool fDumpZKeys);
UniValue importwallet_impl(const JSONRPCRequest& request, bool fImportZKeys);


std::string static EncodeDumpTime(int64_t nTime)
{
    return DateTimeStrFormat("%Y-%m-%dT%H:%M:%SZ", nTime);
}

int64_t static DecodeDumpTime(const std::string& str)
{
    static const boost::posix_time::ptime epoch = boost::posix_time::from_time_t(0);
    static const std::locale loc(std::locale::classic(),
                                 new boost::posix_time::time_input_facet("%Y-%m-%dT%H:%M:%SZ"));
    std::istringstream iss(str);
    iss.imbue(loc);
    boost::posix_time::ptime ptime(boost::date_time::not_a_date_time);
    iss >> ptime;
    if (ptime.is_not_a_date_time())
        return 0;
    return (ptime - epoch).total_seconds();
}

std::string static EncodeDumpString(const std::string& str)
{
    std::stringstream ret;
    BOOST_FOREACH(unsigned char c, str) {
        if (c <= 32 || c >= 128 || c == '%') {
            ret << '%' << HexStr(&c, &c + 1);
        } else {
            ret << c;
        }
    }
    return ret.str();
}

std::string DecodeDumpString(const std::string& str)
{
    std::stringstream ret;
    for (unsigned int pos = 0; pos < str.length(); pos++) {
        unsigned char c = str[pos];
        if (c == '%' && pos + 2 < str.length()) {
            c = (((str[pos + 1] >> 6) * 9 + ((str[pos + 1] - '0') & 15)) << 4) |
                ((str[pos + 2] >> 6) * 9 + ((str[pos + 2] - '0') & 15));
            pos += 2;
        }
        ret << c;
    }
    return ret.str();
}

UniValue _importPrivKey(const string& strPrivKey, const string& strLabel, bool rescan)
{
    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    CBitcoinSecret vchSecret;
    bool fGood = vchSecret.SetString(strPrivKey);

    if (!fGood) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");

    CKey key = vchSecret.GetKey();
    if (!key.IsValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Private key outside allowed range");

    CPubKey pubkey = key.GetPubKey();
    assert(key.VerifyPubKey(pubkey));
    CKeyID vchAddress = pubkey.GetID();
    {
        pwalletMain->MarkDirty();
        pwalletMain->SetAddressBook(vchAddress, strLabel, "receive");

        // Don't throw error in case a key is already there
        if (pwalletMain->HaveKey(vchAddress))
            return EncodeDestination(vchAddress);

        pwalletMain->mapKeyMetadata[vchAddress].nCreateTime = 1;
        pwalletMain->mapKeyMetadata[vchAddress].keyCategory = KeyCategoryLonely;

        if (!pwalletMain->AddKeyPubKey(key, pubkey))
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
        pwalletMain->LearnAllRelatedScripts(pubkey);
//        pwalletMain->ClassifyAddress(vchAddress);

        // whenever a key is imported, we need to scan the whole chain
        pwalletMain->nTimeFirstKey = 1; // 0 would be considered 'no value'

        if (rescan) {
            pwalletMain->ScanForWalletTransactions(chainActive.Genesis(), true);
        }
    }
    return EncodeDestination(vchAddress);;
}

UniValue importprivkey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw runtime_error(
            "importprivkey \"zcashprivkey\" ( \"label\" rescan )\n"
            "\nAdds a private key (as returned by dumpprivkey) to your wallet.\n"
            "\nArguments:\n"
            "1. \"zcashprivkey\"   (string, required) The private key (see dumpprivkey)\n"
            "2. \"label\"            (string, optional, default=\"\") An optional label\n"
            "3. rescan               (boolean, optional, default=true) Rescan the wallet for transactions\n"
            "\nNote: This call can take minutes to complete if rescan is true.\n"
            "\nExamples:\n"
            "\nImport the private key with rescan\n"
            + HelpExampleCli("importprivkey", "\"mykey\"") +
            "\nImport using a label and without rescan\n"
            + HelpExampleCli("importprivkey", "\"mykey\" \"testing\" false") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("importprivkey", "\"mykey\", \"testing\", false")
        );

    string strSecret = request.params[0].get_str();
    string strLabel = "";
    if (request.params.size() > 1)
        strLabel = request.params[1].get_str();

    // Whether to perform rescan after import
    bool fRescan = true;
    if (request.params.size() > 2)
        fRescan = request.params[2].get_bool();

    if (fRescan && fPruneMode)
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled in pruned mode");

    return _importPrivKey(strSecret, strLabel, fRescan);
}

UniValue importpassphrasekey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4)
        throw runtime_error(
            "importpassphrasekey \"zcashprivkey\" ( \"label\" rescan )\n"
            "\nAdds a private key (as returned by exportpassphrasekey) to your wallet.\n"
            "\nArguments:\n"
            "1. \"encryptedprivatekey\"  (string, required) The encrypted private key (see exportpassphrasekey)\n"
            "2. \"passphrase\"           (string, required) The passphrase\n"
            "3. \"label\"                (string, optional, default=\"\") An optional label\n"
            "4. rescan                   (boolean, optional, default=true) Rescan the wallet for transactions\n"
            "\nNote: This call can take minutes to complete if rescan is true.\n"
            "\nExamples:\n"
            "\nImport the private key with rescan\n"
            + HelpExampleCli("importpassphrasekey", "\"encryptedprivatekey\" \"passphrase\"") +
            "\nImport using a label and without rescan\n"
            + HelpExampleCli("importpassphrasekey", "\"encryptedprivatekey\" \"passphrase\" \"label\" false") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("importpassphrasekey", "\"encryptedprivatekey\", \"passphrase\", \"label\", false")
        );

    string strBip38Key = request.params[0].get_str();
    string passphrase = request.params[1].get_str();

    string strLabel = "";
    if (request.params.size() > 2)
        strLabel = request.params[2].get_str();

    // Whether to perform rescan after import
    bool fRescan = true;
    if (request.params.size() > 3)
        fRescan = request.params[3].get_bool();

    if (fRescan && fPruneMode)
        throw JSONRPCError(RPC_WALLET_ERROR, "Rescan is disabled in pruned mode");

//    IsValidBIP38Key(strBip38Key.c_str());

    unsigned char decryptedKey[32] = {0};
    std::string strSecret;
    if (!DecryptBIP38Key(decryptedKey, strSecret, strBip38Key.c_str(), passphrase.c_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid encrypted private key");

    return _importPrivKey(strSecret, strLabel, fRescan);
}

UniValue importaddress(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw runtime_error(
            "importaddress \"address\" ( \"label\" rescan )\n"
            "\nAdds an address or script (in hex) that can be watched as if it were in your wallet but cannot be used to spend.\n"
            "\nArguments:\n"
            "1. \"address\"          (string, required) The address\n"
            "2. \"label\"            (string, optional, default=\"\") An optional label\n"
            "3. rescan               (boolean, optional, default=true) Rescan the wallet for transactions\n"
            "\nNote: This call can take minutes to complete if rescan is true.\n"
            "\nExamples:\n"
            "\nImport an address with rescan\n"
            + HelpExampleCli("importaddress", "\"myaddress\"") +
            "\nImport using a label without rescan\n"
            + HelpExampleCli("importaddress", "\"myaddress\" \"testing\" false") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("importaddress", "\"myaddress\", \"testing\", false")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    CScript script;

    CTxDestination destination = DecodeDestination(request.params[0].get_str());
    if (IsValidDestination(destination)) {
        script = GetScriptForDestination(destination);
    } else if (IsHex(request.params[0].get_str())) {
        std::vector<unsigned char> data(ParseHex(request.params[0].get_str()));
        script = CScript(data.begin(), data.end());
    } else {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Vds address or script");
    }

    string strLabel = "";
    if (request.params.size() > 1)
        strLabel = request.params[1].get_str();

    // Whether to perform rescan after import
    bool fRescan = true;
    if (request.params.size() > 2)
        fRescan = request.params[2].get_bool();

    {
        if (::IsMine(*pwalletMain, script) == ISMINE_SPENDABLE)
            throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");

        // add to address book or update label
        if (IsValidDestination(destination))
            pwalletMain->SetAddressBook(destination, strLabel, "receive");

        // Don't throw error in case an address is already there
        if (pwalletMain->HaveWatchOnly(script))
            return NullUniValue;

        pwalletMain->MarkDirty();

        if (!pwalletMain->AddWatchOnly(script))
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");

        if (fRescan) {
            pwalletMain->ScanForWalletTransactions(chainActive.Genesis(), true);
            pwalletMain->ReacceptWalletTransactions();
        }
    }

    return NullUniValue;
}

UniValue v_importwallet(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "v_importwallet \"filename\"\n"
            "\nImports taddr and zaddr keys from a wallet export file (see v_exportwallet).\n"
            "\nArguments:\n"
            "1. \"filename\"    (string, required) The wallet file\n"
            "\nExamples:\n"
            "\nDump the wallet\n"
            + HelpExampleCli("v_exportwallet", "\"test\"") +
            "\nImport the wallet\n"
            + HelpExampleCli("v_importwallet", "\"test\"") +
            "\nImport using the json rpc call\n"
            + HelpExampleRpc("v_importwallet", "\"test\"")
        );

    return importwallet_impl(request, true);
}

UniValue importwallet(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "importwallet \"filename\"\n"
            "\nImports taddr keys from a wallet dump file (see dumpwallet).\n"
            "\nArguments:\n"
            "1. \"filename\"    (string, required) The wallet file\n"
            "\nExamples:\n"
            "\nDump the wallet\n"
            + HelpExampleCli("dumpwallet", "\"test\"") +
            "\nImport the wallet\n"
            + HelpExampleCli("importwallet", "\"test\"") +
            "\nImport using the json rpc call\n"
            + HelpExampleRpc("importwallet", "\"test\"")
        );

    return importwallet_impl(request, false);
}

UniValue importwallet_impl(const JSONRPCRequest& request, bool fImportZKeys)
{
    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    ifstream file;
    file.open(request.params[0].get_str().c_str(), std::ios::in | std::ios::ate);
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");

    int64_t nTimeBegin = chainActive.Tip()->GetBlockTime();

    bool fGood = true;

    int64_t nFilesize = std::max((int64_t)1, (int64_t)file.tellg());
    file.seekg(0, file.beg);

    pwalletMain->ShowProgress(_("Importing..."), 0); // show progress dialog in GUI
    while (file.good()) {
        pwalletMain->ShowProgress("", std::max(1, std::min(99, (int)(((double)file.tellg() / (double)nFilesize) * 100))));
        std::string line;
        std::getline(file, line);
        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> vstr;
        boost::split(vstr, line, boost::is_any_of(" "));
        if (vstr.size() < 2)
            continue;

        // Let's see if the address is a valid Vds spending key
        if (fImportZKeys) {
            auto spendingkey = DecodeSpendingKey(vstr[0]);
            int64_t nTime = DecodeDumpTime(vstr[1]);
            // Only include hdKeypath and seedFpStr if we have both
            boost::optional<std::string> hdKeypath = (vstr.size() > 3) ? boost::optional<std::string>(vstr[2]) : boost::none;
            boost::optional<std::string> seedFpStr = (vstr.size() > 3) ? boost::optional<std::string>(vstr[3]) : boost::none;
            if (IsValidSpendingKey(spendingkey)) {
                auto addResult = boost::apply_visitor(
                                     AddSpendingKeyToWallet(pwalletMain, Params().GetConsensus(), nTime, hdKeypath, seedFpStr, true), spendingkey);
                if (addResult == KeyAlreadyExists) {
                    LogPrint("vrpc", "Skipping import of zaddr (key already present)\n");
                } else if (addResult == KeyNotAdded) {
                    // Something went wrong
                    fGood = false;
                }
                continue;
            } else {
                LogPrint("vrpc", "Importing detected an error: invalid spending key. Trying as a transparent key...\n");
                // Not a valid spending key, so carry on and see if it's a Vds style t-address.
            }
        }

        CKey key = DecodeSecret(vstr[0]);
        if (!key.IsValid())
            continue;
        CPubKey pubkey = key.GetPubKey();
        assert(key.VerifyPubKey(pubkey));
        CKeyID keyid = pubkey.GetID();
        if (pwalletMain->HaveKey(keyid)) {
            LogPrintf("Skipping import of %s (key already present)\n", EncodeDestination(keyid));
            continue;
        }
        int64_t nTime = DecodeDumpTime(vstr[1]);
        std::string strLabel;
        bool fLabel = true;
        for (unsigned int nStr = 2; nStr < vstr.size(); nStr++) {
            if (boost::algorithm::starts_with(vstr[nStr], "#"))
                break;
            if (vstr[nStr] == "change=1")
                fLabel = false;
            if (vstr[nStr] == "reserve=1")
                fLabel = false;
            if (boost::algorithm::starts_with(vstr[nStr], "label=")) {
                strLabel = DecodeDumpString(vstr[nStr].substr(6));
                fLabel = true;
            }
        }
        LogPrintf("Importing %s...\n", EncodeDestination(keyid));
        if (!pwalletMain->AddKeyPubKey(key, pubkey)) {
            fGood = false;
            continue;
        }
        pwalletMain->mapKeyMetadata[keyid].nCreateTime = nTime;
        if (fLabel)
            pwalletMain->SetAddressBook(keyid, strLabel, "receive");
        nTimeBegin = std::min(nTimeBegin, nTime);
    }
    file.close();
    pwalletMain->ShowProgress("", 100); // hide progress dialog in GUI

    CBlockIndex* pindex = chainActive.Tip();
    while (pindex && pindex->pprev && pindex->GetBlockTime() > nTimeBegin - 7200)
        pindex = pindex->pprev;

    if (!pwalletMain->nTimeFirstKey || nTimeBegin < pwalletMain->nTimeFirstKey)
        pwalletMain->nTimeFirstKey = nTimeBegin;

    LogPrintf("Rescanning last %i blocks\n", chainActive.Height() - pindex->nHeight + 1);
    pwalletMain->ScanForWalletTransactions(pindex);
    pwalletMain->MarkDirty();

    if (!fGood)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding some keys to wallet");

    return NullUniValue;
}

UniValue dumpprivkey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "dumpprivkey \"zcashaddress\"\n"
            "\nReveals the private key corresponding to 'zcashaddress'.\n"
            "Then the importprivkey can be used with this output\n"
            "\nArguments:\n"
            "1. \"zcashaddress\"   (string, required) The zcash address for the private key\n"
            "\nResult:\n"
            "\"key\"                (string) The private key\n"
            "\nExamples:\n"
            + HelpExampleCli("dumpprivkey", "\"myaddress\"")
            + HelpExampleRpc("dumpprivkey", "\"myaddress\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    CTxDestination destination = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(destination))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Vds address");
    const CKeyID* keyID = boost::get<CKeyID>(&destination);
    if (!keyID) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    CKey vchSecret;
    if (!pwalletMain->GetKey(*keyID, vchSecret))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key for address " + request.params[0].get_str() + " is not known");
    return CBitcoinSecret(vchSecret).ToString();
}

UniValue exportpassphrasekey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw runtime_error(
            "exportpassphrasekey \"address\" \"passphrase\"\n"
            "\nReveals and encrypts the private key corresponding to 'address'.\n"
            "Then the importpassphrasekey can be used with this output\n"
            "\nArguments:\n"
            "1. \"address\"     (string, required) The address for the private key\n"
            "2. \"passphrase\"  (string, required) The passphrase to encrypt the private key\n"
            "\nResult:\n"
            "\"key\"            (string) The encrypted private key\n"
            "\nExamples:\n"
            + HelpExampleCli("exportpassphrasekey", "\"address\", \"passphrase\"")
            + HelpExampleRpc("exportpassphrasekey", "\"address\", \"passphrase\"")
        );

    RPCTypeCheck(request.params, boost::assign::list_of(UniValue::VSTR)(UniValue::VSTR));
    if (request.params[0].isNull() || request.params[1].isNull())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, arguments 1 and 2 must be non-null");

    string strAddress = request.params[0].get_str();
    string passphrase = request.params[1].get_str();

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    CTxDestination destination = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(destination))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid Vds address");
    const CKeyID* keyID = boost::get<CKeyID>(&destination);
    if (!keyID) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    CKey vchSecret;
    if (!pwalletMain->GetKey(*keyID, vchSecret))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key for address " + request.params[0].get_str() + " is not known");

    return EncryptBIP38Key(vchSecret, passphrase.c_str());
}

UniValue v_exportwallet(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "v_exportwallet \"filename\"\n"
            "\nExports all wallet keys, for taddr and zaddr, in a human-readable format.\n"
            "\nArguments:\n"
            "1. \"filename\"    (string, required) The filename, saved in folder set by zcashd -exportdir option\n"
            "\nResult:\n"
            "\"path\"           (string) The full path of the destination file\n"
            "\nExamples:\n"
            + HelpExampleCli("v_exportwallet", "\"test\"")
            + HelpExampleRpc("v_exportwallet", "\"test\"")
        );

    return dumpwallet_impl(request, true);
}

UniValue dumpwallet(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "dumpwallet \"filename\"\n"
            "\nDumps taddr wallet keys in a human-readable format.\n"
            "\nArguments:\n"
            "1. \"filename\"    (string, required) The filename, saved in folder set by zcashd -exportdir option\n"
            "\nResult:\n"
            "\"path\"           (string) The full path of the destination file\n"
            "\nExamples:\n"
            + HelpExampleCli("dumpwallet", "\"test\"")
            + HelpExampleRpc("dumpwallet", "\"test\"")
        );

    return dumpwallet_impl(request, false);
}

UniValue dumpwallet_impl(const JSONRPCRequest& request, bool fDumpZKeys)
{
    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    boost::filesystem::path exportdir;
    try {
        exportdir = GetExportDir();
    } catch (const std::runtime_error& e) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, e.what());
    }
    if (exportdir.empty()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot export wallet until the zcashd -exportdir option has been set");
    }
    std::string unclean = request.params[0].get_str();
    std::string clean = SanitizeFilename(unclean);
    if (clean.compare(unclean) != 0) {
        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("Filename is invalid as only alphanumeric characters are allowed.  Try '%s' instead.", clean));
    }
    boost::filesystem::path exportfilepath = exportdir / clean;

    if (boost::filesystem::exists(exportfilepath)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot overwrite existing file " + exportfilepath.string());
    }

    ofstream file;
    file.open(exportfilepath.string().c_str());
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");

    std::map<CKeyID, int64_t> mapKeyBirth;
    std::set<CKeyID> setKeyPool;
    pwalletMain->GetKeyBirthTimes(mapKeyBirth);
    pwalletMain->GetAllReserveKeys(setKeyPool);

    // sort time/key pairs
    std::vector<std::pair<int64_t, CKeyID> > vKeyBirth;
    for (std::map<CKeyID, int64_t>::const_iterator it = mapKeyBirth.begin(); it != mapKeyBirth.end(); it++) {
        vKeyBirth.push_back(std::make_pair(it->second, it->first));
    }
    mapKeyBirth.clear();
    std::sort(vKeyBirth.begin(), vKeyBirth.end());

    // produce output
    file << strprintf("# Wallet dump created by Vds %s (%s)\n", CLIENT_BUILD, CLIENT_DATE);
    file << strprintf("# * Created on %s\n", EncodeDumpTime(GetTime()));
    file << strprintf("# * Best block at time of backup was %i (%s),\n", chainActive.Height(), chainActive.Tip()->GetBlockHash().ToString());
    file << strprintf("#   mined on %s\n", EncodeDumpTime(chainActive.Tip()->GetBlockTime()));
    // add the base58check encoded extended master if the wallet uses HD
    CKeyID masterKeyID = pwalletMain->GetHDChain().masterPubKey.GetID();
    if (!masterKeyID.IsNull()) {
        CKey key;
        if (pwalletMain->GetKey(masterKeyID, key)) {
            CExtKey masterKey;
            masterKey.SetMaster(key.begin(), key.size());

            CBitcoinExtKey b58extkey;
            b58extkey.SetKey(masterKey);

            file << "# extended private masterkey: " << b58extkey.ToString() << "\n\n";
        }
    }
    for (std::vector<std::pair<int64_t, CKeyID> >::const_iterator it = vKeyBirth.begin(); it != vKeyBirth.end(); it++) {
        const CKeyID& keyid = it->second;
        std::string strTime = EncodeDumpTime(it->first);
        std::string strAddr = EncodeDestination(keyid);
        std::string strLabel;
        CKey key;
        if (pwalletMain->GetKey(keyid, key)) {
            if (pwalletMain->mapAddressBook.count(keyid)) {
                file << strprintf("%s %s label=%s # addr=%s\n", EncodeSecret(key), strTime, EncodeDumpString(pwalletMain->mapAddressBook[keyid].name), strAddr);
            } else if (setKeyPool.count(keyid)) {
                file << strprintf("%s %s reserve=1 # addr=%s\n", EncodeSecret(key), strTime, strAddr);
            } else {
                file << strprintf("%s %s change=1 # addr=%s\n", EncodeSecret(key), strTime, strAddr);
            }
        }
    }
    file << "\n";

    if (fDumpZKeys) {
        std::set<libzcash::SaplingPaymentAddress> saplingAddresses;
        pwalletMain->GetSaplingPaymentAddresses(saplingAddresses);
        file << "\n";
        file << "# Sapling keys\n";
        file << "\n";
        for (auto addr : saplingAddresses) {
            libzcash::SaplingExtendedSpendingKey extsk;
            if (pwalletMain->GetSaplingExtendedSpendingKey(addr, extsk)) {
                auto ivk = extsk.expsk.full_viewing_key().in_viewing_key();
                CKeyMetadata keyMeta = pwalletMain->mapSaplingZKeyMetadata[ivk];
                std::string strTime = EncodeDumpTime(keyMeta.nCreateTime);
                // Keys imported with z_importkey do not have zip32 metadata
                if (keyMeta.hdKeypath.empty() ) {
                    file << strprintf("%s %s # zaddr=%s\n", EncodeSpendingKey(extsk), strTime, EncodePaymentAddress(addr));
                } else {
                    file << strprintf("%s %s %s # zaddr=%s\n", EncodeSpendingKey(extsk), strTime, keyMeta.hdKeypath, EncodePaymentAddress(addr));
                }
            }
        }
        file << "\n";
    }

    file << "# End of dump\n";
    file.close();

    return exportfilepath.string();
}


UniValue v_importkey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw runtime_error(
            "v_importkey \"zkey\" ( rescan startHeight )\n"
            "\nAdds a zkey (as returned by v_exportkey) to your wallet.\n"
            "\nArguments:\n"
            "1. \"zkey\"             (string, required) The zkey (see v_exportkey)\n"
            "2. rescan             (string, optional, default=\"whenkeyisnew\") Rescan the wallet for transactions - can be \"yes\", \"no\" or \"whenkeyisnew\"\n"
            "3. startHeight        (numeric, optional, default=0) Block height to start rescan from\n"
            "\nNote: This call can take minutes to complete if rescan is true.\n"
            "\nExamples:\n"
            "\nExport a zkey\n"
            + HelpExampleCli("v_exportkey", "\"myaddress\"") +
            "\nImport the zkey with rescan\n"
            + HelpExampleCli("v_importkey", "\"mykey\"") +
            "\nImport the zkey with partial rescan\n"
            + HelpExampleCli("v_importkey", "\"mykey\" whenkeyisnew 30000") +
            "\nRe-import the zkey with longer partial rescan\n"
            + HelpExampleCli("v_importkey", "\"mykey\" yes 20000") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("v_importkey", "\"mykey\", \"no\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    // Whether to perform rescan after import
    bool fRescan = true;
    bool fIgnoreExistingKey = true;
    if (request.params.size() > 1) {
        auto rescan = request.params[1].get_str();
        if (rescan.compare("whenkeyisnew") != 0) {
            fIgnoreExistingKey = false;
            if (rescan.compare("yes") == 0) {
                fRescan = true;
            } else if (rescan.compare("no") == 0) {
                fRescan = false;
            } else {
                // Handle older API
                UniValue jVal;
                if (!jVal.read(std::string("[") + rescan + std::string("]")) ||
                        !jVal.isArray() || jVal.size() != 1 || !jVal[0].isBool()) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "rescan must be \"yes\", \"no\" or \"whenkeyisnew\"");
                }
                fRescan = jVal[0].getBool();
            }
        }
    }

    // Height to rescan from
    int nRescanHeight = 0;
    if (request.params.size() > 2)
        nRescanHeight = request.params[2].get_int();
    if (nRescanHeight < 0 || nRescanHeight > chainActive.Height()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");
    }

    string strSecret = request.params[0].get_str();
    auto spendingkey = DecodeSpendingKey(strSecret);
    if (!IsValidSpendingKey(spendingkey)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid spending key");
    }

    // Sapling support
    auto addResult = boost::apply_visitor(AddSpendingKeyToWallet(pwalletMain, Params().GetConsensus()), spendingkey);
    if (addResult == KeyAlreadyExists && fIgnoreExistingKey) {
        return NullUniValue;
    }
    pwalletMain->MarkDirty();
    if (addResult == KeyNotAdded) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding spending key to wallet");
    }

    // whenever a key is imported, we need to scan the whole chain
    pwalletMain->nTimeFirstKey = 1; // 0 would be considered 'no value'

    // We want to scan for transactions and notes
    if (fRescan) {
        pwalletMain->ScanForWalletTransactions(chainActive[nRescanHeight], true);
    }

    return NullUniValue;
}

UniValue v_importviewingkey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw runtime_error(
            "v_importviewingkey \"vkey\" ( rescan startHeight )\n"
            "\nAdds a viewing key (as returned by z_exportviewingkey) to your wallet.\n"
            "\nArguments:\n"
            "1. \"vkey\"             (string, required) The viewing key (see z_exportviewingkey)\n"
            "2. rescan             (string, optional, default=\"whenkeyisnew\") Rescan the wallet for transactions - can be \"yes\", \"no\" or \"whenkeyisnew\"\n"
            "3. startHeight        (numeric, optional, default=0) Block height to start rescan from\n"
            "\nNote: This call can take minutes to complete if rescan is true.\n"
            "\nExamples:\n"
            "\nImport a viewing key\n"
            + HelpExampleCli("v_importviewingkey", "\"vkey\"") +
            "\nImport the viewing key without rescan\n"
            + HelpExampleCli("v_importviewingkey", "\"vkey\", no") +
            "\nImport the viewing key with partial rescan\n"
            + HelpExampleCli("v_importviewingkey", "\"vkey\" whenkeyisnew 30000") +
            "\nRe-import the viewing key with longer partial rescan\n"
            + HelpExampleCli("v_importviewingkey", "\"vkey\" yes 20000") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("v_importviewingkey", "\"vkey\", \"no\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    // Whether to perform rescan after import
    bool fRescan = true;
    bool fIgnoreExistingKey = true;
    if (request.params.size() > 1) {
        auto rescan = request.params[1].get_str();
        if (rescan.compare("whenkeyisnew") != 0) {
            fIgnoreExistingKey = false;
            if (rescan.compare("no") == 0) {
                fRescan = false;
            } else if (rescan.compare("yes") != 0) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "rescan must be \"yes\", \"no\" or \"whenkeyisnew\"");
            }
        }
    }

    // Height to rescan from
//    int nRescanHeight = 0;
//    if (request.params.size() > 2) {
//        nRescanHeight = request.params[2].get_int();
//    }
//    if (nRescanHeight < 0 || nRescanHeight > chainActive.Height()) {
//        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");
//    }

//    string strVKey = request.params[0].get_str();
//    auto viewingkey = DecodeViewingKey(strVKey);
//    if (!IsValidViewingKey(viewingkey)) {
//        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid viewing key");
//    }
    // TODO: Add Sapling support. For now, return an error to the user.
//    if (boost::get<libzcash::SproutViewingKey>(&viewingkey) == nullptr) {
//        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Currently, only Sprout viewing keys are supported");
//    }
//    auto vkey = boost::get<libzcash::SproutViewingKey>(viewingkey);
//    auto addr = vkey.address();

//    {
//        if (pwalletMain->HaveSproutSpendingKey(addr)) {
//            throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this viewing key");
//        }

//        // Don't throw error in case a viewing key is already there
//        if (pwalletMain->HaveSproutViewingKey(addr)) {
//            if (fIgnoreExistingKey) {
//                return NullUniValue;
//            }
//        } else {
//            pwalletMain->MarkDirty();

//            if (!pwalletMain->AddSproutViewingKey(vkey)) {
//                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding viewing key to wallet");
//            }
//        }

//        // We want to scan for transactions and notes
//        if (fRescan) {
//            pwalletMain->ScanForWalletTransactions(chainActive[nRescanHeight], true);
//        }
//    }

    return NullUniValue;
}



UniValue v_exportkey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "v_exportkey \"zaddr\"\n"
            "\nReveals the zkey corresponding to 'zaddr'.\n"
            "Then the v_importkey can be used with this output\n"
            "\nArguments:\n"
            "1. \"zaddr\"   (string, required) The zaddr for the private key\n"
            "\nResult:\n"
            "\"key\"                  (string) The private key\n"
            "\nExamples:\n"
            + HelpExampleCli("v_exportkey", "\"myaddress\"")
            + HelpExampleCli("v_importkey", "\"mykey\"")
            + HelpExampleRpc("v_exportkey", "\"myaddress\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    string strAddress = request.params[0].get_str();

    auto address = DecodePaymentAddress(strAddress);
    if (!IsValidPaymentAddress(address)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid zaddr");
    }

    // Sapling support
    auto sk = boost::apply_visitor(GetSpendingKeyForPaymentAddress(pwalletMain), address);
    if (!sk) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet does not hold private zkey for this zaddr");
    }
    return EncodeSpendingKey(sk.get());
}

UniValue v_exportviewingkey(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "v_exportviewingkey \"zaddr\"\n"
            "\nReveals the viewing key corresponding to 'zaddr'.\n"
            "Then the v_importviewingkey can be used with this output\n"
            "\nArguments:\n"
            "1. \"zaddr\"   (string, required) The zaddr for the viewing key\n"
            "\nResult:\n"
            "\"vkey\"                  (string) The viewing key\n"
            "\nExamples:\n"
            + HelpExampleCli("v_exportviewingkey", "\"myaddress\"")
            + HelpExampleRpc("v_exportviewingkey", "\"myaddress\"")
        );

    LOCK2(cs_main, pwalletMain->cs_wallet);

    EnsureWalletIsUnlocked();

    string strAddress = request.params[0].get_str();

    auto address = DecodePaymentAddress(strAddress);
    if (!IsValidPaymentAddress(address)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid zaddr");
    }
    // TODO: Add Sapling support. For now, return an error to the user.
//    if (boost::get<libzcash::SproutPaymentAddress>(&address) == nullptr) {
//        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Currently, only Sprout zaddrs are supported");
//    }
//    auto addr = boost::get<libzcash::SproutPaymentAddress>(address);

//    libzcash::SproutViewingKey vk;
//    if (!pwalletMain->GetSproutViewingKey(addr, vk)) {
//        libzcash::SproutSpendingKey k;
//        if (!pwalletMain->GetSproutSpendingKey(addr, k)) {
//            throw JSONRPCError(RPC_WALLET_ERROR, "Wallet does not hold private key or viewing key for this zaddr");
//        }
//        vk = k.viewing_key();
//    }

//    return EncodeViewingKey(vk);
    return NullUniValue;
}
