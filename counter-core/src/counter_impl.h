#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "logos_module_context.h"

struct WalletHandle;

/**
 * @brief One number on the Logos Execution Zone that everyone shares.
 *
 * The counter is a tiny LEZ program (counter-program/) deployed once to the
 * public testnet. Its value lives in the program's own shard of a public PDA
 * derived from the program's address, so every copy of this module — on any
 * machine — reads and increments the same number, the way every Ethereum
 * client sees the same contract storage.
 *
 * Each install has its own embedded LEZ wallet. Incrementing is a public
 * transaction; the fee (a sliver of LGO) is topped up automatically from the
 * testnet's shared faucet account.
 *
 * Universal module: these public methods are the API. Wallet work happens on
 * one worker thread; `state()` returns a cached snapshot the UI polls.
 */
class LezCounterImpl : public LogosModuleContext
{
public:
    LezCounterImpl();
    ~LezCounterImpl();

    /// Everything the UI draws, as one JSON object. Never blocks.
    std::string state();
    /// Add `by` (1–100) to the shared counter.
    std::string increment(int64_t by);
    /// Top up this wallet's LGO from the testnet faucet.
    std::string getGas();
    /// Point at a different deployment of the counter program (base58 address);
    /// empty resets to the built-in one. For after a testnet reset.
    std::string setProgram(const std::string& address);
    /// Deploy a counter program binary (path to counter.bin) from this wallet
    /// and switch to it. Prints the new address in the job's result.
    std::string deploy(const std::string& binPath);

protected:
    void onContextReady() override;

private:
    using Bytes32 = std::array<uint8_t, 32>;
    using u128 = unsigned __int128;

    struct Job {
        int64_t     id = 0;
        std::string kind;
        std::string label;
        std::string status;      // queued, working, confirming, done, failed
        std::string tx;
        std::string error;
        std::string result;
        int64_t     createdMs = 0;
        int64_t     finishedMs = 0;
        std::function<void(Job&)> run;
    };

    struct Change {
        int64_t  by = 0;
        uint64_t value = 0;
        int64_t  atMs = 0;
        bool     mine = false;
    };

    void start();
    void workerLoop();
    void openWallet();
    void refreshNow();
    std::string enqueue(const std::string& kind, const std::string& label, std::function<void(Job&)> run);
    void setJob(int64_t id, const std::function<void(Job&)>& f);
    void confirm(const std::string& txHash, int64_t jobId);

    bool readShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out);
    uint64_t readValue(const Bytes32& program);
    u128 nativeBalance(const Bytes32& account);
    Bytes32 counterAccount(const Bytes32& program);
    std::string toBase58(const Bytes32& id);
    bool fromBase58(const std::string& s, Bytes32& out);
    void ensureGas(u128 atLeast, u128 topUp);

    std::string dataDir() const;
    void loadMeta();
    void saveMeta();

    std::thread             m_worker;
    std::atomic<bool>       m_running{false};
    std::mutex              m_qmu;
    std::condition_variable m_qcv;
    std::deque<int64_t>     m_queue;
    std::atomic<bool>       m_kick{false};
    std::atomic<bool>       m_dirty{false};

    WalletHandle* m_wallet = nullptr;   // worker thread only

    mutable std::mutex m_mu;            // guards everything below
    std::string m_phase = "starting";
    std::string m_error;
    int64_t     m_block = 0;
    bool        m_online = false;

    bool        m_haveMe = false;
    Bytes32     m_me{};
    std::string m_meB58;
    u128        m_gas = 0;
    bool        m_haveFaucet = false;

    std::string m_programOverride;      // base58; empty = built-in
    Bytes32     m_program{};
    std::string m_programB58, m_counterB58;
    bool        m_programOk = false;
    bool        m_seen = false;
    uint64_t    m_value = 0;
    uint64_t    m_pendingMine = 0;      // our own increments not yet seen on chain
    std::vector<Change> m_changes;

    std::vector<Job> m_jobs;
    int64_t          m_nextJob = 1;
};
