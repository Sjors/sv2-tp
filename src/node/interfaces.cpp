// Copyright (c) 2018-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <init.h>
#include <interfaces/chain.h>
#include <interfaces/handler.h>
#include <interfaces/mining.h>
#include <interfaces/node.h>
#include <interfaces/types.h>
#include <kernel/chain.h>
#include <kernel/context.h>
#include <kernel/mempool_entry.h>
#include <logging.h>
#include <net.h>
#include <netaddress.h>
#include <netbase.h>
#include <node/blockstorage.h>
#include <node/coin.h>
#include <node/context.h>
#include <node/interface_ui.h>
#include <node/mini_miner.h>
#include <node/miner.h>
#include <node/kernel_notifications.h>
#include <node/transaction.h>
#include <node/types.h>
#include <node/warnings.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <policy/settings.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <support/allocators/secure.h>
#include <sync.h>
#include <txmempool.h>
#include <uint256.h>
#include <univalue.h>
#include <util/check.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/string.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <any>
#include <memory>
#include <optional>
#include <utility>

#include <boost/signals2/signal.hpp>

using interfaces::BlockRef;
using interfaces::BlockTemplate;
using interfaces::BlockTip;
using interfaces::Chain;
using interfaces::FoundBlock;
using interfaces::Handler;
using interfaces::MakeSignalHandler;
using interfaces::Mining;
using interfaces::Node;
using node::BlockAssembler;
using node::BlockWaitOptions;
using util::Join;

namespace node {
// All members of the classes in this namespace are intentionally public, as the
// classes themselves are private.
namespace {

class NodeImpl : public Node
{
public:
    explicit NodeImpl(NodeContext& context) { setContext(&context); }
    void initLogging() override { InitLogging(args()); }
    void initParameterInteraction() override { InitParameterInteraction(args()); }
    bilingual_str getWarnings() override { return Join(Assert(m_context->warnings)->GetMessages(), Untranslated("<hr />")); }
    int getExitStatus() override { return Assert(m_context)->exit_status.load(); }
    BCLog::CategoryMask getLogCategories() override { return LogInstance().GetCategoryMask(); }
    bool baseInitialize() override
    {
        if (!AppInitBasicSetup(args(), Assert(context())->exit_status)) return false;
        if (!AppInitParameterInteraction(args())) return false;

        m_context->warnings = std::make_unique<node::Warnings>();
        m_context->kernel = std::make_unique<kernel::Context>();
        m_context->ecc_context = std::make_unique<ECC_Context>();
        if (!AppInitSanityChecks(*m_context->kernel)) return false;

        if (!AppInitLockDirectories()) return false;
        if (!AppInitInterfaces(*m_context)) return false;

        return true;
    }
    bool appInitMain(interfaces::BlockAndHeaderTipInfo* tip_info) override
    {
        if (AppInitMain(*m_context, tip_info)) return true;
        // Error during initialization, set exit status before continue
        m_context->exit_status.store(EXIT_FAILURE);
        return false;
    }
    void appShutdown() override
    {
        Interrupt(*m_context);
        Shutdown(*m_context);
    }
    void startShutdown() override
    {
        NodeContext& ctx{*Assert(m_context)};
        if (!(Assert(ctx.shutdown_request))()) {
            LogError("Failed to send shutdown signal\n");
        }
    }
    bool shutdownRequested() override { return ShutdownRequested(*Assert(m_context)); };
    bool isSettingIgnored(const std::string& name) override
    {
        bool ignored = false;
        args().LockSettings([&](common::Settings& settings) {
            if (auto* options = common::FindKey(settings.command_line_options, name)) {
                ignored = !options->empty();
            }
        });
        return ignored;
    }
    common::SettingsValue getPersistentSetting(const std::string& name) override { return args().GetPersistentSetting(name); }
    void updateRwSetting(const std::string& name, const common::SettingsValue& value) override
    {
        args().LockSettings([&](common::Settings& settings) {
            if (value.isNull()) {
                settings.rw_settings.erase(name);
            } else {
                settings.rw_settings[name] = value;
            }
        });
        args().WriteSettingsFile();
    }
    void forceSetting(const std::string& name, const common::SettingsValue& value) override
    {
        args().LockSettings([&](common::Settings& settings) {
            if (value.isNull()) {
                settings.forced_settings.erase(name);
            } else {
                settings.forced_settings[name] = value;
            }
        });
    }
    void resetSettings() override
    {
        args().WriteSettingsFile(/*errors=*/nullptr, /*backup=*/true);
        args().LockSettings([&](common::Settings& settings) {
            settings.rw_settings.clear();
        });
        args().WriteSettingsFile();
    }
    bool getProxy(Network net, Proxy& proxy_info) override { return GetProxy(net, proxy_info); }
    size_t getNodeCount(ConnectionDirection flags) override
    {
        return 0;
    }
    bool getBanned(banmap_t& banmap) override
    {
        return false;
    }
    bool ban(const CNetAddr& net_addr, int64_t ban_time_offset) override
    {
        return false;
    }
    bool unban(const CSubNet& ip) override
    {
        return false;
    }
    bool disconnectByAddress(const CNetAddr& net_addr) override
    {
        return false;
    }
    bool disconnectById(NodeId id) override
    {
        return false;
    }
    std::vector<std::unique_ptr<interfaces::ExternalSigner>> listExternalSigners() override
    {
        return {};
    }
    int64_t getTotalBytesRecv() override { return 0; }
    int64_t getTotalBytesSent() override { return 0; }
    size_t getMempoolSize() override { return m_context->mempool ? m_context->mempool->size() : 0; }
    size_t getMempoolDynamicUsage() override { return m_context->mempool ? m_context->mempool->DynamicMemoryUsage() : 0; }
    size_t getMempoolMaxUsage() override { return m_context->mempool ? m_context->mempool->m_opts.max_size_bytes : 0; }
    bool getHeaderTip(int& height, int64_t& block_time) override
    {
        LOCK(::cs_main);
        auto best_header = chainman().m_best_header;
        if (best_header) {
            height = best_header->nHeight;
            block_time = best_header->GetBlockTime();
            return true;
        }
        return false;
    }
    std::map<CNetAddr, LocalServiceInfo> getNetLocalAddresses() override
    {
        return {};
    }
    int getNumBlocks() override
    {
        LOCK(::cs_main);
        return chainman().ActiveChain().Height();
    }
    uint256 getBestBlockHash() override
    {
        const CBlockIndex* tip = WITH_LOCK(::cs_main, return chainman().ActiveChain().Tip());
        return tip ? tip->GetBlockHash() : chainman().GetParams().GenesisBlock().GetHash();
    }
    int64_t getLastBlockTime() override
    {
        LOCK(::cs_main);
        if (chainman().ActiveChain().Tip()) {
            return chainman().ActiveChain().Tip()->GetBlockTime();
        }
        return chainman().GetParams().GenesisBlock().GetBlockTime(); // Genesis block's time of current network
    }
    double getVerificationProgress() override
    {
        LOCK(chainman().GetMutex());
        return chainman().GuessVerificationProgress(chainman().ActiveTip());
    }
    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }
    bool isLoadingBlocks() override { return chainman().m_blockman.LoadingBlocks(); }
    void setNetworkActive(bool active) override {}
    bool getNetworkActive() override { return false; }
    CFeeRate getDustRelayFee() override
    {
        if (!m_context->mempool) return CFeeRate{DUST_RELAY_TX_FEE};
        return m_context->mempool->m_opts.dust_relay_feerate;
    }
    std::optional<Coin> getUnspentOutput(const COutPoint& output) override
    {
        LOCK(::cs_main);
        return chainman().ActiveChainstate().CoinsTip().GetCoin(output);
    }
    TransactionError broadcastTransaction(CTransactionRef tx, CAmount max_tx_fee, std::string& err_string) override
    {
        return BroadcastTransaction(*m_context, std::move(tx), err_string, max_tx_fee, /*relay=*/ true, /*wait_callback=*/ false);
    }
    std::unique_ptr<Handler> handleInitMessage(InitMessageFn fn) override
    {
        return MakeSignalHandler(::uiInterface.InitMessage_connect(fn));
    }
    std::unique_ptr<Handler> handleMessageBox(MessageBoxFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ThreadSafeMessageBox_connect(fn));
    }
    std::unique_ptr<Handler> handleQuestion(QuestionFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ThreadSafeQuestion_connect(fn));
    }
    std::unique_ptr<Handler> handleShowProgress(ShowProgressFn fn) override
    {
        return MakeSignalHandler(::uiInterface.ShowProgress_connect(fn));
    }
    std::unique_ptr<Handler> handleInitWallet(InitWalletFn fn) override
    {
        return MakeSignalHandler(::uiInterface.InitWallet_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyNumConnectionsChanged(NotifyNumConnectionsChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyNumConnectionsChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyNetworkActiveChanged(NotifyNetworkActiveChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyNetworkActiveChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyAlertChanged(NotifyAlertChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyAlertChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleBannedListChanged(BannedListChangedFn fn) override
    {
        return MakeSignalHandler(::uiInterface.BannedListChanged_connect(fn));
    }
    std::unique_ptr<Handler> handleNotifyBlockTip(NotifyBlockTipFn fn) override
    {
        return MakeSignalHandler(::uiInterface.NotifyBlockTip_connect([fn](SynchronizationState sync_state, const CBlockIndex& block, double verification_progress) {
            fn(sync_state, BlockTip{block.nHeight, block.GetBlockTime(), block.GetBlockHash()}, verification_progress);
        }));
    }
    std::unique_ptr<Handler> handleNotifyHeaderTip(NotifyHeaderTipFn fn) override
    {
        return MakeSignalHandler(
            ::uiInterface.NotifyHeaderTip_connect([fn](SynchronizationState sync_state, int64_t height, int64_t timestamp, bool presync) {
                fn(sync_state, BlockTip{(int)height, timestamp, uint256{}}, presync);
            }));
    }
    NodeContext* context() override { return m_context; }
    void setContext(NodeContext* context) override
    {
        m_context = context;
    }
    ArgsManager& args() { return *Assert(Assert(m_context)->args); }
    ChainstateManager& chainman() { return *Assert(m_context->chainman); }
    NodeContext* m_context{nullptr};
};

// NOLINTNEXTLINE(misc-no-recursion)
bool FillBlock(const CBlockIndex* index, const FoundBlock& block, UniqueLock<RecursiveMutex>& lock, const CChain& active, const BlockManager& blockman) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (!index) return false;
    if (block.m_hash) *block.m_hash = index->GetBlockHash();
    if (block.m_height) *block.m_height = index->nHeight;
    if (block.m_time) *block.m_time = index->GetBlockTime();
    if (block.m_max_time) *block.m_max_time = index->GetBlockTimeMax();
    if (block.m_mtp_time) *block.m_mtp_time = index->GetMedianTimePast();
    if (block.m_in_active_chain) *block.m_in_active_chain = active[index->nHeight] == index;
    if (block.m_locator) { *block.m_locator = GetLocator(index); }
    if (block.m_next_block) FillBlock(active[index->nHeight] == index ? active[index->nHeight + 1] : nullptr, *block.m_next_block, lock, active, blockman);
    if (block.m_data) {
        REVERSE_LOCK(lock, cs_main);
        if (!blockman.ReadBlock(*block.m_data, *index)) block.m_data->SetNull();
    }
    block.found = true;
    return true;
}

class NotificationsProxy : public CValidationInterface
{
public:
    explicit NotificationsProxy(std::shared_ptr<Chain::Notifications> notifications)
        : m_notifications(std::move(notifications)) {}
    virtual ~NotificationsProxy() = default;
    void TransactionAddedToMempool(const NewMempoolTransactionInfo& tx, uint64_t mempool_sequence) override
    {
        m_notifications->transactionAddedToMempool(tx.info.m_tx);
    }
    void TransactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason, uint64_t mempool_sequence) override
    {
        m_notifications->transactionRemovedFromMempool(tx, reason);
    }
    void BlockConnected(ChainstateRole role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        m_notifications->blockConnected(role, kernel::MakeBlockInfo(index, block.get()));
    }
    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* index) override
    {
        m_notifications->blockDisconnected(kernel::MakeBlockInfo(index, block.get()));
    }
    void UpdatedBlockTip(const CBlockIndex* index, const CBlockIndex* fork_index, bool is_ibd) override
    {
        m_notifications->updatedBlockTip();
    }
    void ChainStateFlushed(ChainstateRole role, const CBlockLocator& locator) override {
        m_notifications->chainStateFlushed(role, locator);
    }
    std::shared_ptr<Chain::Notifications> m_notifications;
};

class NotificationsHandlerImpl : public Handler
{
public:
    explicit NotificationsHandlerImpl(ValidationSignals& signals, std::shared_ptr<Chain::Notifications> notifications)
        : m_signals{signals}, m_proxy{std::make_shared<NotificationsProxy>(std::move(notifications))}
    {
        m_signals.RegisterSharedValidationInterface(m_proxy);
    }
    ~NotificationsHandlerImpl() override { disconnect(); }
    void disconnect() override
    {
        if (m_proxy) {
            m_signals.UnregisterSharedValidationInterface(m_proxy);
            m_proxy.reset();
        }
    }
    ValidationSignals& m_signals;
    std::shared_ptr<NotificationsProxy> m_proxy;
};

class ChainImpl : public Chain
{
public:
    explicit ChainImpl(NodeContext& node) : m_node(node) {}
    std::optional<int> getHeight() override
    {
        const int height{WITH_LOCK(::cs_main, return chainman().ActiveChain().Height())};
        return height >= 0 ? std::optional{height} : std::nullopt;
    }
    uint256 getBlockHash(int height) override
    {
        LOCK(::cs_main);
        return Assert(chainman().ActiveChain()[height])->GetBlockHash();
    }
    bool haveBlockOnDisk(int height) override
    {
        LOCK(::cs_main);
        const CBlockIndex* block{chainman().ActiveChain()[height]};
        return block && ((block->nStatus & BLOCK_HAVE_DATA) != 0) && block->nTx > 0;
    }
    std::optional<int> findLocatorFork(const CBlockLocator& locator) override
    {
        LOCK(::cs_main);
        if (const CBlockIndex* fork = chainman().ActiveChainstate().FindForkInGlobalIndex(locator)) {
            return fork->nHeight;
        }
        return std::nullopt;
    }
    bool findBlock(const uint256& hash, const FoundBlock& block) override
    {
        WAIT_LOCK(cs_main, lock);
        return FillBlock(chainman().m_blockman.LookupBlockIndex(hash), block, lock, chainman().ActiveChain(), chainman().m_blockman);
    }
    bool findFirstBlockWithTimeAndHeight(int64_t min_time, int min_height, const FoundBlock& block) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        return FillBlock(active.FindEarliestAtLeast(min_time, min_height), block, lock, active, chainman().m_blockman);
    }
    bool findAncestorByHeight(const uint256& block_hash, int ancestor_height, const FoundBlock& ancestor_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        if (const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash)) {
            if (const CBlockIndex* ancestor = block->GetAncestor(ancestor_height)) {
                return FillBlock(ancestor, ancestor_out, lock, active, chainman().m_blockman);
            }
        }
        return FillBlock(nullptr, ancestor_out, lock, active, chainman().m_blockman);
    }
    bool findAncestorByHash(const uint256& block_hash, const uint256& ancestor_hash, const FoundBlock& ancestor_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash);
        const CBlockIndex* ancestor = chainman().m_blockman.LookupBlockIndex(ancestor_hash);
        if (block && ancestor && block->GetAncestor(ancestor->nHeight) != ancestor) ancestor = nullptr;
        return FillBlock(ancestor, ancestor_out, lock, chainman().ActiveChain(), chainman().m_blockman);
    }
    bool findCommonAncestor(const uint256& block_hash1, const uint256& block_hash2, const FoundBlock& ancestor_out, const FoundBlock& block1_out, const FoundBlock& block2_out) override
    {
        WAIT_LOCK(cs_main, lock);
        const CChain& active = chainman().ActiveChain();
        const CBlockIndex* block1 = chainman().m_blockman.LookupBlockIndex(block_hash1);
        const CBlockIndex* block2 = chainman().m_blockman.LookupBlockIndex(block_hash2);
        const CBlockIndex* ancestor = block1 && block2 ? LastCommonAncestor(block1, block2) : nullptr;
        // Using & instead of && below to avoid short circuiting and leaving
        // output uninitialized. Cast bool to int to avoid -Wbitwise-instead-of-logical
        // compiler warnings.
        return int{FillBlock(ancestor, ancestor_out, lock, active, chainman().m_blockman)} &
               int{FillBlock(block1, block1_out, lock, active, chainman().m_blockman)} &
               int{FillBlock(block2, block2_out, lock, active, chainman().m_blockman)};
    }
    void findCoins(std::map<COutPoint, Coin>& coins) override { return FindCoins(m_node, coins); }
    double guessVerificationProgress(const uint256& block_hash) override
    {
        LOCK(chainman().GetMutex());
        return chainman().GuessVerificationProgress(chainman().m_blockman.LookupBlockIndex(block_hash));
    }
    bool hasBlocks(const uint256& block_hash, int min_height, std::optional<int> max_height) override
    {
        // hasBlocks returns true if all ancestors of block_hash in specified
        // range have block data (are not pruned), false if any ancestors in
        // specified range are missing data.
        //
        // For simplicity and robustness, min_height and max_height are only
        // used to limit the range, and passing min_height that's too low or
        // max_height that's too high will not crash or change the result.
        LOCK(::cs_main);
        if (const CBlockIndex* block = chainman().m_blockman.LookupBlockIndex(block_hash)) {
            if (max_height && block->nHeight >= *max_height) block = block->GetAncestor(*max_height);
            for (; block->nStatus & BLOCK_HAVE_DATA; block = block->pprev) {
                // Check pprev to not segfault if min_height is too low
                if (block->nHeight <= min_height || !block->pprev) return true;
            }
        }
        return false;
    }
    RBFTransactionState isRBFOptIn(const CTransaction& tx) override
    {
        if (!m_node.mempool) return IsRBFOptInEmptyMempool(tx);
        LOCK(m_node.mempool->cs);
        return IsRBFOptIn(tx, *m_node.mempool);
    }
    bool isInMempool(const Txid& txid) override
    {
        if (!m_node.mempool) return false;
        LOCK(m_node.mempool->cs);
        return m_node.mempool->exists(txid);
    }
    bool hasDescendantsInMempool(const Txid& txid) override
    {
        if (!m_node.mempool) return false;
        LOCK(m_node.mempool->cs);
        const auto entry{m_node.mempool->GetEntry(txid)};
        if (entry == nullptr) return false;
        return entry->GetCountWithDescendants() > 1;
    }
    bool broadcastTransaction(const CTransactionRef& tx,
        const CAmount& max_tx_fee,
        bool relay,
        std::string& err_string) override
    {
        const TransactionError err = BroadcastTransaction(m_node, tx, err_string, max_tx_fee, relay, /*wait_callback=*/false);
        // Chain clients only care about failures to accept the tx to the mempool. Disregard non-mempool related failures.
        // Note: this will need to be updated if BroadcastTransactions() is updated to return other non-mempool failures
        // that Chain clients do not need to know about.
        return TransactionError::OK == err;
    }
    void getTransactionAncestry(const Txid& txid, size_t& ancestors, size_t& descendants, size_t* ancestorsize, CAmount* ancestorfees) override
    {
        ancestors = descendants = 0;
        if (!m_node.mempool) return;
        m_node.mempool->GetTransactionAncestry(txid, ancestors, descendants, ancestorsize, ancestorfees);
    }

    std::map<COutPoint, CAmount> calculateIndividualBumpFees(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) override
    {
        if (!m_node.mempool) {
            std::map<COutPoint, CAmount> bump_fees;
            for (const auto& outpoint : outpoints) {
                bump_fees.emplace(outpoint, 0);
            }
            return bump_fees;
        }
        return MiniMiner(*m_node.mempool, outpoints).CalculateBumpFees(target_feerate);
    }

    std::optional<CAmount> calculateCombinedBumpFee(const std::vector<COutPoint>& outpoints, const CFeeRate& target_feerate) override
    {
        if (!m_node.mempool) {
            return 0;
        }
        return MiniMiner(*m_node.mempool, outpoints).CalculateTotalBumpFees(target_feerate);
    }
    void getPackageLimits(unsigned int& limit_ancestor_count, unsigned int& limit_descendant_count) override
    {
        const CTxMemPool::Limits default_limits{};

        const CTxMemPool::Limits& limits{m_node.mempool ? m_node.mempool->m_opts.limits : default_limits};

        limit_ancestor_count = limits.ancestor_count;
        limit_descendant_count = limits.descendant_count;
    }
    util::Result<void> checkChainLimits(const CTransactionRef& tx) override
    {
        if (!m_node.mempool) return {};
        LockPoints lp;
        CTxMemPoolEntry entry(tx, 0, 0, 0, 0, false, 0, lp);
        LOCK(m_node.mempool->cs);
        return m_node.mempool->CheckPackageLimits({tx}, entry.GetTxSize());
    }
    CFeeRate estimateSmartFee(int num_blocks, bool conservative, FeeCalculation* calc) override
    {
        if (!m_node.fee_estimator) return {};
        return m_node.fee_estimator->estimateSmartFee(num_blocks, calc, conservative);
    }
    unsigned int estimateMaxBlocks() override
    {
        if (!m_node.fee_estimator) return 0;
        return m_node.fee_estimator->HighestTargetTracked(FeeEstimateHorizon::LONG_HALFLIFE);
    }
    CFeeRate mempoolMinFee() override
    {
        if (!m_node.mempool) return {};
        return m_node.mempool->GetMinFee();
    }
    CFeeRate relayMinFee() override
    {
        if (!m_node.mempool) return CFeeRate{DEFAULT_MIN_RELAY_TX_FEE};
        return m_node.mempool->m_opts.min_relay_feerate;
    }
    CFeeRate relayIncrementalFee() override
    {
        if (!m_node.mempool) return CFeeRate{DEFAULT_INCREMENTAL_RELAY_FEE};
        return m_node.mempool->m_opts.incremental_relay_feerate;
    }
    CFeeRate relayDustFee() override
    {
        if (!m_node.mempool) return CFeeRate{DUST_RELAY_TX_FEE};
        return m_node.mempool->m_opts.dust_relay_feerate;
    }
    bool havePruned() override
    {
        LOCK(::cs_main);
        return chainman().m_blockman.m_have_pruned;
    }
    bool isReadyToBroadcast() override { return !chainman().m_blockman.LoadingBlocks() && !isInitialBlockDownload(); }
    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }
    bool shutdownRequested() override { return ShutdownRequested(m_node); }
    void initMessage(const std::string& message) override { ::uiInterface.InitMessage(message); }
    void initWarning(const bilingual_str& message) override { InitWarning(message); }
    void initError(const bilingual_str& message) override { InitError(message); }
    void showProgress(const std::string& title, int progress, bool resume_possible) override
    {
        ::uiInterface.ShowProgress(title, progress, resume_possible);
    }
    std::unique_ptr<Handler> handleNotifications(std::shared_ptr<Notifications> notifications) override
    {
        return std::make_unique<NotificationsHandlerImpl>(validation_signals(), std::move(notifications));
    }
    void waitForNotificationsIfTipChanged(const uint256& old_tip) override
    {
        if (!old_tip.IsNull() && old_tip == WITH_LOCK(::cs_main, return chainman().ActiveChain().Tip()->GetBlockHash())) return;
        validation_signals().SyncWithValidationInterfaceQueue();
    }
    common::SettingsValue getSetting(const std::string& name) override
    {
        return args().GetSetting(name);
    }
    std::vector<common::SettingsValue> getSettingsList(const std::string& name) override
    {
        return args().GetSettingsList(name);
    }
    common::SettingsValue getRwSetting(const std::string& name) override
    {
        common::SettingsValue result;
        args().LockSettings([&](const common::Settings& settings) {
            if (const common::SettingsValue* value = common::FindKey(settings.rw_settings, name)) {
                result = *value;
            }
        });
        return result;
    }
    bool updateRwSetting(const std::string& name,
                         const interfaces::SettingsUpdate& update_settings_func) override
    {
        std::optional<interfaces::SettingsAction> action;
        args().LockSettings([&](common::Settings& settings) {
            if (auto* value = common::FindKey(settings.rw_settings, name)) {
                action = update_settings_func(*value);
                if (value->isNull()) settings.rw_settings.erase(name);
            } else {
                UniValue new_value;
                action = update_settings_func(new_value);
                if (!new_value.isNull()) settings.rw_settings[name] = std::move(new_value);
            }
        });
        if (!action) return false;
        // Now dump value to disk if requested
        return *action != interfaces::SettingsAction::WRITE || args().WriteSettingsFile();
    }
    bool overwriteRwSetting(const std::string& name, common::SettingsValue value, interfaces::SettingsAction action) override
    {
        return updateRwSetting(name, [&](common::SettingsValue& settings) {
            settings = std::move(value);
            return action;
        });
    }
    bool deleteRwSettings(const std::string& name, interfaces::SettingsAction action) override
    {
        return overwriteRwSetting(name, {}, action);
    }
    void requestMempoolTransactions(Notifications& notifications) override
    {
        if (!m_node.mempool) return;
        LOCK2(::cs_main, m_node.mempool->cs);
        for (const CTxMemPoolEntry& entry : m_node.mempool->entryAll()) {
            notifications.transactionAddedToMempool(entry.GetSharedTx());
        }
    }
    bool hasAssumedValidChain() override
    {
        return chainman().IsSnapshotActive();
    }

    NodeContext* context() override { return &m_node; }
    ArgsManager& args() { return *Assert(m_node.args); }
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    ValidationSignals& validation_signals() { return *Assert(m_node.validation_signals); }
    NodeContext& m_node;
};

class BlockTemplateImpl : public BlockTemplate
{
public:
    explicit BlockTemplateImpl(BlockAssembler::Options assemble_options,
                               std::unique_ptr<CBlockTemplate> block_template,
                               NodeContext& node) : m_assemble_options(std::move(assemble_options)),
                                                    m_block_template(std::move(block_template)),
                                                    m_node(node)
    {
        assert(m_block_template);
    }

    CBlockHeader getBlockHeader() override
    {
        return m_block_template->block;
    }

    CBlock getBlock() override
    {
        return m_block_template->block;
    }

    std::vector<CAmount> getTxFees() override
    {
        return m_block_template->vTxFees;
    }

    std::vector<int64_t> getTxSigops() override
    {
        return m_block_template->vTxSigOpsCost;
    }

    CTransactionRef getCoinbaseTx() override
    {
        return m_block_template->block.vtx[0];
    }

    std::vector<unsigned char> getCoinbaseCommitment() override
    {
        return m_block_template->vchCoinbaseCommitment;
    }

    int getWitnessCommitmentIndex() override
    {
        return GetWitnessCommitmentIndex(m_block_template->block);
    }

    std::vector<uint256> getCoinbaseMerklePath() override
    {
        return TransactionMerklePath(m_block_template->block, 0);
    }

    bool submitSolution(uint32_t version, uint32_t timestamp, uint32_t nonce, CTransactionRef coinbase) override
    {
        AddMerkleRootAndCoinbase(m_block_template->block, std::move(coinbase), version, timestamp, nonce);
        return chainman().ProcessNewBlock(std::make_shared<const CBlock>(m_block_template->block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/nullptr);
    }

    std::unique_ptr<BlockTemplate> waitNext(BlockWaitOptions options) override
    {
        auto new_template = WaitAndCreateNewBlock(chainman(), notifications(), m_node.mempool.get(), m_block_template, options, m_assemble_options);
        if (new_template) return std::make_unique<BlockTemplateImpl>(m_assemble_options, std::move(new_template), m_node);
        return nullptr;
    }

    const BlockAssembler::Options m_assemble_options;

    const std::unique_ptr<CBlockTemplate> m_block_template;

    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    KernelNotifications& notifications() { return *Assert(m_node.notifications); }
    NodeContext& m_node;
};

class MinerImpl : public Mining
{
public:
    explicit MinerImpl(NodeContext& node) : m_node(node) {}

    bool isTestChain() override
    {
        return chainman().GetParams().IsTestChain();
    }

    bool isInitialBlockDownload() override
    {
        return chainman().IsInitialBlockDownload();
    }

    std::optional<BlockRef> getTip() override
    {
        return GetTip(chainman());
    }

    std::optional<BlockRef> waitTipChanged(uint256 current_tip, MillisecondsDouble timeout) override
    {
        return WaitTipChanged(chainman(), notifications(), current_tip, timeout);
    }

    std::unique_ptr<BlockTemplate> createNewBlock(const BlockCreateOptions& options) override
    {
        // Ensure m_tip_block is set so consumers of BlockTemplate can rely on that.
        if (!waitTipChanged(uint256::ZERO, MillisecondsDouble::max())) return {};

        BlockAssembler::Options assemble_options{options};
        ApplyArgsManOptions(*Assert(m_node.args), assemble_options);
        return std::make_unique<BlockTemplateImpl>(assemble_options, BlockAssembler{chainman().ActiveChainstate(), context()->mempool.get(), assemble_options}.CreateNewBlock(), m_node);
    }

    bool checkBlock(const CBlock& block, const node::BlockCheckOptions& options, std::string& reason, std::string& debug) override
    {
        LOCK(chainman().GetMutex());
        BlockValidationState state{TestBlockValidity(chainman().ActiveChainstate(), block, /*check_pow=*/options.check_pow, /*=check_merkle_root=*/options.check_merkle_root)};
        reason = state.GetRejectReason();
        debug = state.GetDebugMessage();
        return state.IsValid();
    }

    NodeContext* context() override { return &m_node; }
    ChainstateManager& chainman() { return *Assert(m_node.chainman); }
    KernelNotifications& notifications() { return *Assert(m_node.notifications); }
    NodeContext& m_node;
};
} // namespace
} // namespace node

namespace interfaces {
std::unique_ptr<Node> MakeNode(node::NodeContext& context) { return std::make_unique<node::NodeImpl>(context); }
std::unique_ptr<Chain> MakeChain(node::NodeContext& context) { return std::make_unique<node::ChainImpl>(context); }
std::unique_ptr<Mining> MakeMining(node::NodeContext& context) { return std::make_unique<node::MinerImpl>(context); }
} // namespace interfaces
