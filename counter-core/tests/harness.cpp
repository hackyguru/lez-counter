// lezcounter harness: drives LezCounterImpl against the testnet, outside Basecamp.
//
//   harness state                 print the counter and this wallet
//   harness deploy counter.bin    deploy the program from this wallet, switch to it
//   harness use <address>         point at a deployed program
//   harness increment [N]         add N (default 1) and wait for it
//
// The data dir comes from LEZCOUNTER_DATA_DIR, so two dirs = two people.
#include "counter_impl.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using nlohmann::json;

static json stateOf(LezCounterImpl& m) { return json::parse(m.state()); }

static void waitReady(LezCounterImpl& m)
{
    for (int i = 0; i < 240; ++i) {
        const json s = stateOf(m);
        if (s["phase"] == "error") { fprintf(stderr, "wallet error: %s\n", s["error"].get<std::string>().c_str()); exit(2); }
        if (s["phase"] == "ready" && s["network"]["block"].get<int64_t>() > 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    fprintf(stderr, "wallet never became ready\n");
    exit(2);
}

static json waitJob(LezCounterImpl& m, const std::string& reply)
{
    const json r = json::parse(reply);
    if (!r.value("ok", false)) { fprintf(stderr, "refused: %s\n", r.value("error", "").c_str()); exit(3); }
    const int64_t id = r["job"];
    std::string last;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const json s = stateOf(m);
        for (const auto& j : s["jobs"]) {
            if (j["id"] != id) continue;
            const std::string st = j["status"];
            if (st != last) {
                const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count();
                printf("  [%3llds] %-10s %s\n", (long long)secs, st.c_str(), j["label"].get<std::string>().c_str());
                fflush(stdout);
                last = st;
            }
            if (st == "done" || st == "failed") {
                if (st == "failed") printf("  error: %s\n", j["error"].get<std::string>().c_str());
                if (!j["tx"].get<std::string>().empty()) printf("  tx: %s\n", j["tx"].get<std::string>().c_str());
                if (!j["result"].get<std::string>().empty()) printf("  result: %s\n", j["result"].get<std::string>().c_str());
                std::this_thread::sleep_for(std::chrono::seconds(6));
                return j;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

int main(int argc, char** argv)
{
    const std::string cmd = argc > 1 ? argv[1] : "state";
    LezCounterImpl m;
    waitReady(m);
    json job;
    if (cmd == "deploy" && argc > 2) job = waitJob(m, m.deploy(argv[2]));
    else if (cmd == "use" && argc > 2) { m.setProgram(argv[2]); std::this_thread::sleep_for(std::chrono::seconds(6)); }
    else if (cmd == "increment") job = waitJob(m, m.increment(argc > 2 ? std::stoll(argv[2]) : 1));
    else if (cmd != "state") { fprintf(stderr, "usage: harness state | deploy BIN | use ADDRESS | increment [N]\n"); return 64; }
    else std::this_thread::sleep_for(std::chrono::seconds(5));

    const json s = stateOf(m);
    printf("%s\n", json{{"value", s["value"]}, {"program", s["program"]}, {"me", s["me"]},
                        {"block", s["network"]["block"]}, {"changes", s["changes"]}}.dump(2).c_str());
    fflush(stdout);
    return job.is_object() && job["status"] == "failed" ? 1 : 0;
}
