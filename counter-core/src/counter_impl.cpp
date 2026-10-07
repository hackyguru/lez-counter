#include "counter_impl.h"

#include <nlohmann/json.hpp>

extern "C" {
#include "wallet_ffi.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

// The counter program on the LEZ v0.3 public testnet, deployed 2026-10-02 from
// counter-program/counter.bin (image id 4316d160…). After a testnet reset,
// redeploy (tests/run.sh deploy) and either update this or use setProgram() /
// the UI's settings.
constexpr const char* kDefaultProgram = "5ShXhcA9R972B2BgbomP2Qi3JM1gxGw7Yf5NHwA1fvv4";

// Every client finds the counter at the public PDA of (program, this seed).
// The program would accept any account; this is the convention that makes
// everyone share one number.
constexpr const char kCounterSeed[] = "lez-counter/shared/v1";

// The testnet's shared faucet: a genesis account whose key is public on
// purpose (`PRIVATE_KEY_PUB_ACC_B` in lez/testnet_initial_state).
constexpr const char* kFaucetKeyHex = "717940b1cc55e5d6b2066dbf1d9a3f26f212f4db08d02388177fcfedd8a9be1b";
constexpr const char* kFaucetB58 = "7wHg9sbJwc6h3NP1S9bekfAzB8CHifEcxKswCKUt3YQo";

using u128 = unsigned __int128;
constexpr u128 kLgo = 1000000000;
constexpr int64_t kTickMs = 4000;
constexpr int64_t kConfirmTimeoutMs = 4 * 60 * 1000;

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s.push_back(d[p[i] >> 4]); s.push_back(d[p[i] & 15]); }
    return s;
}

bool unhex(const std::string& s, uint8_t* out, size_t n)
{
    if (s.size() != n * 2) return false;
    auto v = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < n; ++i) {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = uint8_t(hi * 16 + lo);
    }
    return true;
}

FfiBytes32 ffi(const std::array<uint8_t, 32>& b)
{
    FfiBytes32 f{};
    std::copy(b.begin(), b.end(), f.data);
    return f;
}

std::array<uint8_t, 32> fromFfi(const FfiBytes32& f)
{
    std::array<uint8_t, 32> b{};
    std::copy(std::begin(f.data), std::end(f.data), b.begin());
    return b;
}

u128 le128(const uint8_t* p)
{
    u128 v = 0;
    for (int i = 15; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

std::string formatLgo(u128 atomic)
{
    auto dec = [](u128 v) {
        if (!v) return std::string("0");
        std::string s;
        while (v) { s.insert(s.begin(), char('0' + int(v % 10))); v /= 10; }
        return s;
    };
    std::string frac = dec(atomic % kLgo);
    frac.insert(frac.begin(), 9 - frac.size(), '0');
    frac.resize(4);
    while (frac.size() > 2 && frac.back() == '0') frac.pop_back();
    return dec(atomic / kLgo) + "." + frac;
}

std::string errorText(int code)
{
    switch (code) {
    case NETWORK_ERROR:      return "Couldn't reach the LEZ sequencer.";
    case INSUFFICIENT_FUNDS: return "Not enough LGO for the fee.";
    case PAYER_CANNOT_FUND:  return "Not enough LGO for the fee — tap Get LGO.";
    case INVALID_BYTECODE:   return "That file isn't a valid LEZ program.";
    case INTERNAL_ERROR:     return "The wallet couldn't build that transaction (error 99) — the Basecamp log has details.";
    default:                 return "Wallet error " + std::to_string(code) + ".";
    }
}

void check(WalletFfiError e, const char* what)
{
    if (e != SUCCESS) {
        fprintf(stderr, "[lezcounter] %s failed: %d\n", what, int(e));
        throw std::runtime_error(errorText(int(e)));
    }
}

std::string jsonOk(const json& extra = json::object())
{
    json j = extra;
    j["ok"] = true;
    return j.dump();
}

std::string jsonErr(const std::string& msg)
{
    return json{{"ok", false}, {"error", msg}}.dump();
}

} // namespace


LezCounterImpl::LezCounterImpl() = default;

LezCounterImpl::~LezCounterImpl()
{
    m_running = false;
    m_qcv.notify_all();
    if (m_worker.joinable()) m_worker.join();
    if (m_wallet) {
        wallet_ffi_save(m_wallet);
        wallet_ffi_destroy(m_wallet);
    }
}

void LezCounterImpl::onContextReady() { start(); }

void LezCounterImpl::start()
{
    if (m_running.exchange(true)) return;
    m_worker = std::thread([this] { workerLoop(); });
}

std::string LezCounterImpl::dataDir() const
{
    if (const char* env = std::getenv("LEZCOUNTER_DATA_DIR"); env && *env) return env;
    if (isContextReady() && !instancePersistencePath().empty()) return instancePersistencePath();
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/.lezcounter";
}

void LezCounterImpl::loadMeta()
{
    std::ifstream f(dataDir() + "/lezcounter.json");
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (j.is_discarded()) return;
    std::lock_guard<std::mutex> lk(m_mu);
    m_haveMe = unhex(j.value("me", ""), m_me.data(), 32);
    m_haveFaucet = j.value("faucetImported", false);
    m_programOverride = j.value("program", "");
}

void LezCounterImpl::saveMeta()
{
    json j;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_haveMe) j["me"] = hex(m_me.data(), 32);
        j["faucetImported"] = m_haveFaucet;
        j["program"] = m_programOverride;
    }
    const std::string path = dataDir() + "/lezcounter.json";
    std::ofstream(path + ".tmp", std::ios::trunc) << j.dump(2);
    std::error_code ec;
    fs::rename(path + ".tmp", path, ec);
}

// ── worker ──────────────────────────────────────────────────────────────

void LezCounterImpl::workerLoop()
{
    try {
        openWallet();
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(m_mu);
        m_phase = "error";
        m_error = e.what();
        return;
    }

    int64_t lastRefresh = 0;
    while (m_running) {
        int64_t jobId = 0;
        {
            std::unique_lock<std::mutex> lk(m_qmu);
            m_qcv.wait_for(lk, std::chrono::milliseconds(400),
                           [this] { return !m_running || !m_queue.empty() || m_kick; });
            if (!m_running) break;
            if (!m_queue.empty()) { jobId = m_queue.front(); m_queue.pop_front(); }
        }
        if (jobId) {
            std::function<void(Job&)> run;
            {
                std::lock_guard<std::mutex> lk(m_mu);
                for (auto& j : m_jobs) if (j.id == jobId) { j.status = "working"; run = j.run; }
            }
            Job scratch;
            scratch.id = jobId;
            try {
                if (run) run(scratch);
                setJob(jobId, [](Job& j) { j.status = "done"; j.finishedMs = nowMs(); });
            } catch (const std::exception& e) {
                setJob(jobId, [&](Job& j) { j.status = "failed"; j.error = e.what(); j.finishedMs = nowMs(); });
            }
            wallet_ffi_save(m_wallet);
            saveMeta();
            lastRefresh = 0;
        }
        if (m_dirty.exchange(false)) saveMeta();
        if (m_kick.exchange(false) || nowMs() - lastRefresh >= kTickMs) {
            try {
                refreshNow();
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = true;
            } catch (const std::exception& e) {
                std::lock_guard<std::mutex> lk(m_mu);
                m_online = false;
                fprintf(stderr, "[lezcounter] refresh: %s\n", e.what());
            }
            lastRefresh = nowMs();
        }
    }
}

void LezCounterImpl::openWallet()
{
    const std::string dir = dataDir();
    fs::create_directories(dir + "/wallet");
    loadMeta();
    const std::string config = dir + "/wallet/config.json";
    const std::string storage = dir + "/wallet/storage.json";
    const std::string stats = dir + "/wallet/statistics.json";

    if (!fs::exists(config)) {
        json cfg = {
            {"sequencers", json::array({{{"sequencer_addr", "https://testnet.lez.logos.co"}}})},
            {"seq_poll_timeout", "30s"},
            {"seq_tx_poll_max_blocks", 8},
            {"seq_poll_max_retries", 8},
            {"seq_block_poll_max_amount", 100},
        };
        std::ofstream(config) << cfg.dump(2);
    }
    if (fs::exists(storage)) {
        m_wallet = wallet_ffi_open(config.c_str(), storage.c_str(), stats.c_str());
        if (!m_wallet) throw std::runtime_error("Couldn't open the wallet in " + dir);
    } else {
        // Nothing of value lives in this wallet (a little faucet LGO), so the
        // password is random and the recovery words aren't kept.
        std::random_device rd;
        uint8_t raw[24];
        for (auto& b : raw) b = uint8_t(rd() & 0xff);
        const std::string password = hex(raw, sizeof raw);
        FfiCreateWalletOutput out = wallet_ffi_create_new(config.c_str(), storage.c_str(), stats.c_str(), password.c_str());
        if (!out.wallet) throw std::runtime_error("Couldn't create a wallet in " + dir);
        m_wallet = out.wallet;
        if (out.mnemonic) wallet_ffi_free_string(out.mnemonic);
        std::lock_guard<std::mutex> lk(m_mu);
        m_haveMe = false;
        m_haveFaucet = false;
    }
    if (!m_haveMe) {
        FfiBytes32 id{};
        check(wallet_ffi_create_account_public(m_wallet, &id), "create_account_public");
        std::lock_guard<std::mutex> lk(m_mu);
        m_me = fromFfi(id);
        m_haveMe = true;
    }
    const std::string me58 = toBase58(m_me);
    {
        std::lock_guard<std::mutex> lk(m_mu);
        m_meB58 = me58;
        m_phase = "ready";
    }
    wallet_ffi_save(m_wallet);
    saveMeta();
}

void LezCounterImpl::refreshNow()
{
    uint64_t height = 0;
    check(wallet_ffi_get_current_block_height(m_wallet, &height), "get_current_block_height");

    std::string programStr;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        programStr = m_programOverride.empty() ? kDefaultProgram : m_programOverride;
    }
    Bytes32 program{};
    const bool ok = !programStr.empty() && fromBase58(programStr, program);
    const std::string counter58 = ok ? toBase58(counterAccount(program)) : "";
    const uint64_t value = ok ? readValue(program) : 0;
    const u128 gas = nativeBalance(m_me);

    std::lock_guard<std::mutex> lk(m_mu);
    m_block = int64_t(height);
    m_gas = gas;
    if (!ok || program != m_program || !m_programOk) {
        m_seen = false;
        m_changes.clear();
        m_pendingMine = 0;
    }
    m_program = program;
    m_programOk = ok;
    m_programB58 = ok ? programStr : "";
    m_counterB58 = counter58;
    if (!ok) return;
    if (m_seen && value > m_value) {
        // Rises we caused ourselves are labelled ours; anything beyond is someone else.
        uint64_t delta = value - m_value;
        const uint64_t mine = std::min(delta, m_pendingMine);
        m_pendingMine -= mine;
        if (mine) m_changes.insert(m_changes.begin(), {int64_t(mine), m_value + mine, nowMs(), true});
        if (delta > mine) m_changes.insert(m_changes.begin(), {int64_t(delta - mine), value, nowMs(), false});
        if (m_changes.size() > 50) m_changes.resize(50);
    }
    m_value = value;
    m_seen = true;
}

// ── jobs ────────────────────────────────────────────────────────────────

std::string LezCounterImpl::enqueue(const std::string& kind, const std::string& label, std::function<void(Job&)> run)
{
    int64_t id;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_phase != "ready") return jsonErr("The wallet is still starting.");
        Job j;
        j.id = id = m_nextJob++;
        j.kind = kind;
        j.label = label;
        j.status = "queued";
        j.createdMs = nowMs();
        j.run = std::move(run);
        m_jobs.insert(m_jobs.begin(), std::move(j));
        if (m_jobs.size() > 30) m_jobs.resize(30);
    }
    {
        std::lock_guard<std::mutex> lk(m_qmu);
        m_queue.push_back(id);
    }
    m_qcv.notify_all();
    return jsonOk({{"job", id}});
}

void LezCounterImpl::setJob(int64_t id, const std::function<void(Job&)>& f)
{
    std::lock_guard<std::mutex> lk(m_mu);
    for (auto& j : m_jobs) if (j.id == id) { f(j); return; }
}

void LezCounterImpl::confirm(const std::string& txHash, int64_t jobId)
{
    FfiBytes32 h{};
    const bool isHash = unhex(txHash, h.data, 32);
    setJob(jobId, [&](Job& j) { if (isHash) j.tx = txHash; j.status = "confirming"; });
    if (!isHash) return;   // program_loader_deploy reports the header id here and has already waited
    const int64_t deadline = nowMs() + kConfirmTimeoutMs;
    while (m_running && nowMs() < deadline) {
        bool included = false;
        if (wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included) return;
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    throw std::runtime_error("Sent, but not seen in a block after 4 minutes.");
}

// ── chain reads ─────────────────────────────────────────────────────────

std::string LezCounterImpl::toBase58(const Bytes32& id)
{
    const FfiBytes32 f = ffi(id);
    char* s = wallet_ffi_account_id_to_base58(&f);
    if (!s) return hex(id.data(), 32);
    std::string out = s;
    wallet_ffi_free_string(s);
    return out;
}

bool LezCounterImpl::fromBase58(const std::string& in, Bytes32& out)
{
    std::string s;
    for (char c : in) if (c != ' ' && c != '\n' && c != '\t') s.push_back(c);
    if (s.rfind("Public/", 0) == 0) s = s.substr(7);
    FfiBytes32 f{};
    if (s.empty() || wallet_ffi_account_id_from_base58(s.c_str(), &f) != SUCCESS) return false;
    out = fromFfi(f);
    return true;
}

LezCounterImpl::Bytes32 LezCounterImpl::counterAccount(const Bytes32& program)
{
    FfiPdaSeed seed{};
    std::copy(std::begin(kCounterSeed), std::end(kCounterSeed) - 1, seed.data);   // zero-padded to 32
    return fromFfi(wallet_ffi_account_id_for_public_pda(ffi(program), seed));
}

bool LezCounterImpl::readShard(const Bytes32& account, const Bytes32& program, std::vector<uint8_t>& out)
{
    out.clear();
    const FfiBytes32 id = ffi(account);
    FfiAccount acc{};
    if (wallet_ffi_get_account_public(m_wallet, &id, &acc) != SUCCESS) return false;
    bool found = false;
    for (uintptr_t i = 0; i < acc.shards_len; ++i) {
        if (std::equal(program.begin(), program.end(), acc.shards[i].program.data)) {
            out.assign(acc.shards[i].data, acc.shards[i].data + acc.shards[i].data_len);
            found = true;
            break;
        }
    }
    wallet_ffi_free_account_data(&acc);
    return found;
}

uint64_t LezCounterImpl::readValue(const Bytes32& program)
{
    std::vector<uint8_t> raw;
    if (!readShard(counterAccount(program), program, raw) || raw.size() != 8) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | raw[i];
    return v;
}

u128 LezCounterImpl::nativeBalance(const Bytes32& account)
{
    const FfiBytes32 id = ffi(account);
    uint8_t out[16] = {};
    if (wallet_ffi_get_balance(m_wallet, &id, true, &out) != SUCCESS) return 0;
    return le128(out);
}

void LezCounterImpl::ensureGas(u128 atLeast, u128 topUp)
{
    if (nativeBalance(m_me) >= atLeast) return;
    Bytes32 faucet{};
    if (!fromBase58(kFaucetB58, faucet)) throw std::runtime_error("Bad faucet address.");
    if (!m_haveFaucet) {
        wallet_ffi_import_public_account(m_wallet, kFaucetKeyHex);   // harmless if already there
        std::lock_guard<std::mutex> lk(m_mu);
        m_haveFaucet = true;
    }
    // Everyone shares this faucet account, so simultaneous top-ups collide on
    // its nonce; retry a couple of blocks later.
    std::string lastErr;
    for (int attempt = 0; attempt < 4; ++attempt) {
        try {
            const FfiBytes32 f = ffi(faucet), t = ffi(m_me);
            uint8_t amount[16] = {};
            u128 v = topUp;
            for (int i = 0; i < 16; ++i) { amount[i] = uint8_t(v & 0xff); v >>= 8; }
            FfiTransferResult res{};
            check(wallet_ffi_transfer_public(m_wallet, &f, &t, &amount, &res), "transfer_public");
            const std::string tx = res.tx_hash ? res.tx_hash : "";
            wallet_ffi_free_transfer_result(&res);
            FfiBytes32 h{};
            if (unhex(tx, h.data, 32)) {
                const int64_t deadline = nowMs() + kConfirmTimeoutMs;
                bool included = false;
                while (nowMs() < deadline &&
                       !(wallet_ffi_poll_transaction_status(m_wallet, h, &included) == SUCCESS && included))
                    std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            if (nativeBalance(m_me) >= atLeast) return;
        } catch (const std::exception& e) {
            lastErr = e.what();
        }
        std::this_thread::sleep_for(std::chrono::seconds(15 + 10 * attempt));
    }
    throw std::runtime_error("Couldn't get LGO from the testnet faucet" + (lastErr.empty() ? "." : ": " + lastErr));
}

// ── API ─────────────────────────────────────────────────────────────────

std::string LezCounterImpl::state()
{
    start();
    std::lock_guard<std::mutex> lk(m_mu);
    json changes = json::array();
    for (const auto& c : m_changes)
        changes.push_back({{"by", c.by}, {"value", c.value}, {"at", c.atMs}, {"mine", c.mine}});
    json jobs = json::array();
    for (const auto& j : m_jobs)
        jobs.push_back({{"id", j.id}, {"kind", j.kind}, {"label", j.label}, {"status", j.status}, {"tx", j.tx},
                        {"error", j.error}, {"result", j.result}, {"created", j.createdMs}, {"finished", j.finishedMs}});
    return json{
        {"phase", m_phase},
        {"error", m_error},
        {"network", {{"block", m_block}, {"online", m_online}}},
        {"program", {{"address", m_programB58}, {"counter", m_counterB58}, {"configured", m_programOk},
                     {"custom", !m_programOverride.empty()}}},
        {"value", m_seen ? json(m_value) : json(nullptr)},
        {"changes", changes},
        {"me", {{"address", m_meB58}, {"lgo", formatLgo(m_gas)}, {"hasGas", m_gas >= kLgo / 100}}},
        {"jobs", jobs},
    }.dump();
}

std::string LezCounterImpl::increment(int64_t by)
{
    if (by < 1 || by > 100) return jsonErr("Increment by 1 to 100.");
    Bytes32 program{};
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_programOk) return jsonErr("No counter program configured yet.");
        program = m_program;
    }
    return enqueue("increment", "+" + std::to_string(by), [this, by, program](Job& job) {
        ensureGas(kLgo / 100, kLgo);
        const uint64_t start = readValue(program);

        FfiAccountMention m{};
        check(wallet_ffi_resolve_public_account(ffi(counterAccount(program)), false, &m.identity), "resolve_public_account");
        m.program_account_id = ffi(program);
        uint8_t instruction[8];
        for (int i = 0; i < 8; ++i) instruction[i] = uint8_t((uint64_t(by) >> (8 * i)) & 0xff);   // Borsh u64
        const FfiBytes32 payer = ffi(m_me);
        FfiTransactionResult res{};
        const WalletFfiError e = wallet_ffi_send_generic_public_transaction(
            m_wallet, &m, 1, instruction, sizeof instruction, ffi(program), &payer, &res);
        wallet_ffi_free_account_identity(&m.identity);
        check(e, "send_generic_public_transaction");
        const std::string tx = res.tx_hash ? res.tx_hash : "";
        const bool ok = res.success;
        wallet_ffi_free_transaction_result(&res);
        if (!ok) throw std::runtime_error("The sequencer rejected the transaction.");
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_pendingMine += uint64_t(by);
        }
        confirm(tx, job.id);

        // Inclusion isn't success: a program that panics still lands in a block
        // and pays its fee. The counter having moved by at least `by` is.
        for (int i = 0; i < 6; ++i) {
            if (readValue(program) >= start + uint64_t(by)) return;
            std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        std::lock_guard<std::mutex> lk(m_mu);
        m_pendingMine -= std::min(m_pendingMine, uint64_t(by));
        throw std::runtime_error("The transaction landed but the counter program rejected it.");
    });
}

std::string LezCounterImpl::getGas()
{
    return enqueue("gas", "Get LGO", [this](Job&) { ensureGas(kLgo / 2, kLgo); });
}

std::string LezCounterImpl::setProgram(const std::string& address)
{
    std::lock_guard<std::mutex> lk(m_mu);
    std::string a;
    for (char c : address) if (c != ' ' && c != '\n' && c != '\t') a.push_back(c);
    m_programOverride = a;
    m_dirty = true;
    m_kick = true;
    m_qcv.notify_all();
    return jsonOk();
}

std::string LezCounterImpl::deploy(const std::string& binPath)
{
    std::ifstream f(binPath, std::ios::binary);
    if (!f) return jsonErr("Can't read " + binPath);
    std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (elf.empty()) return jsonErr(binPath + " is empty.");
    return enqueue("deploy", "Deploy counter program", [this, elf](Job& job) {
        ensureGas(2 * kLgo, 5 * kLgo);
        // program_loader stores the program in write-once 96 KiB segments plus
        // a header; the header's account id is the program's address.
        constexpr size_t kSegment = 96 * 1024;
        const size_t segments = (elf.size() + kSegment - 1) / kSegment;
        FfiBytes32 header{};
        check(wallet_ffi_create_account_public(m_wallet, &header), "create_account_public");
        std::vector<FfiBytes32> segs(segments);
        for (auto& s : segs) check(wallet_ffi_create_account_public(m_wallet, &s), "create_account_public");
        wallet_ffi_save(m_wallet);
        const FfiBytes32 payer = ffi(m_me);
        FfiTransactionResult res{};
        check(wallet_ffi_program_loader_deploy(m_wallet, &header, segs.data(), segs.size(), elf.data(), elf.size(),
                                               true, &payer, &res),
              "program_loader_deploy");
        const std::string tx = res.tx_hash ? res.tx_hash : "";
        const bool ok = res.success;
        wallet_ffi_free_transaction_result(&res);
        if (!ok) throw std::runtime_error("The deployment was rejected.");
        confirm(tx, job.id);
        const std::string address = toBase58(fromFfi(header));
        setJob(job.id, [&](Job& j) { j.result = address; });
        std::lock_guard<std::mutex> lk(m_mu);
        m_programOverride = address;
        m_kick = true;
    });
}
