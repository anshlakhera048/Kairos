// Tournament driver for the Phase 4 arena.
//
// Runs each bot on each seed, collects results, scores, and outputs JSON.
// Usage: kairos_tournament --bots as,fixed,momentum,random --seeds 50
//        --duration 60 --regime calm --out results.json

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "kairos/arena.hpp"
#include "strategies/arena_bots.hpp"
#include "strategies/avellaneda_stoikov.hpp"

using namespace kairos;
using namespace kairos::arena;

namespace {

// Run a single bot on a single seed. Returns the BotResult.
template <typename Bot>
BotResult run_one(const std::string& bot_name, Bot& bot,
                  const ArenaConfig& base_cfg, std::uint64_t seed,
                  const Regime& regime) {
    ArenaConfig cfg = base_cfg;
    cfg.seed = seed;
    Arena<Bot> arena(cfg);
    // Flow generators (deterministic from seed).
    NoiseConfig nc = regime.noise;
    arena.add_generator(std::make_unique<NoiseTrader>(nc, 10));
    InformedConfig ic = regime.informed;
    arena.add_generator(
        std::make_unique<InformedTrader>(ic, 10000, 11));
    FixedSpreadConfig fc;
    arena.add_generator(std::make_unique<FixedSpreadMaker>(fc, 12));
    MomentumConfig mc;
    arena.add_generator(std::make_unique<MomentumTrader>(mc, 13));
    return arena.run(bot, bot_name);
}

void print_usage(const char* prog) {
    std::printf("usage: %s --bots <list> --seeds <n> --duration <s>\n", prog);
    std::printf("       --regime <calm|volatile|informed_heavy> --out <file>\n");
    std::printf("  bots: comma-separated from as,fixed,momentum,random,cheater\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string bots_str = "as,fixed,momentum,random";
    int n_seeds = 5;
    int duration_s = 60;
    std::string regime_str = "calm";
    std::string out_path = "tournament.json";

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bots") == 0 && i + 1 < argc) {
            bots_str = argv[++i];
        } else if (std::strcmp(argv[i], "--seeds") == 0 && i + 1 < argc) {
            n_seeds = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration_s = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--regime") == 0 && i + 1 < argc) {
            regime_str = argv[++i];
        } else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    // Parse bot list.
    std::vector<std::string> bot_names;
    std::size_t pos = 0;
    while (pos < bots_str.size()) {
        const std::size_t comma = bots_str.find(',', pos);
        bot_names.push_back(
            bots_str.substr(pos, comma == std::string::npos
                                      ? std::string::npos
                                      : comma - pos));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    ArenaConfig base_cfg;
    base_cfg.duration_ns = static_cast<std::uint64_t>(duration_s) * 1'000'000'000ULL;

    // Run all bots on all seeds. Collect results as JSON.
    std::FILE* out = std::fopen(out_path.c_str(), "w");
    if (!out) {
        std::printf("cannot open %s\n", out_path.c_str());
        return 1;
    }
    std::fprintf(out, "[\n");
    bool first = true;

    for (const auto& name : bot_names) {
        for (int s = 0; s < n_seeds; ++s) {
            const std::uint64_t seed = 1000 + static_cast<std::uint64_t>(s);
            Regime regime;
            if (regime_str == "volatile") {
                regime = regime_volatile(seed);
            } else if (regime_str == "informed_heavy") {
                regime = regime_informed_heavy(seed);
            } else {
                regime = regime_calm(seed);
            }

            BotResult res;
            if (name == "as") {
                ASStrategyConfig scfg;
                scfg.model.gamma = 1500.0;
                scfg.model.sigma = 0.0002;
                scfg.model.kappa = 2.0;
                scfg.model.horizon_s = 3600.0;
                scfg.order_size_lots = 1'000'000;
                scfg.max_inventory_lots = 10'000'000;
                scfg.max_spread_ticks = 100;
                ASStrategy bot(scfg);
                res = run_one(name, bot, base_cfg, seed, regime);
            } else if (name == "fixed") {
                bots::FixedSpreadBot bot;
                res = run_one(name, bot, base_cfg, seed, regime);
            } else if (name == "momentum") {
                bots::MomentumBot bot;
                res = run_one(name, bot, base_cfg, seed, regime);
            } else if (name == "random") {
                bots::RandomBot bot(seed);
                res = run_one(name, bot, base_cfg, seed, regime);
            } else if (name == "cheater") {
                bots::CheaterBot bot;
                res = run_one(name, bot, base_cfg, seed, regime);
            } else {
                std::printf("unknown bot: %s\n", name.c_str());
                continue;
            }

            if (!first) std::fprintf(out, ",\n");
            first = false;
            std::fprintf(out,
                         "  {\"bot\": \"%s\", \"seed\": %llu, "
                         "\"fills\": %llu, \"acks\": %llu, \"rejects\": %llu, "
                         "\"position\": %lld, \"pnl_lots\": %lld, "
                         "\"disqualified\": %s, \"dq_reason\": \"%s\"}",
                         res.bot_name.c_str(),
                         (unsigned long long)res.seed,
                         (unsigned long long)res.fills,
                         (unsigned long long)res.acks,
                         (unsigned long long)res.rejects,
                         (long long)res.final_position_lots,
                         (long long)res.realized_pnl_lots,
                         res.disqualified ? "true" : "false",
                         res.dq_reason.c_str());
            std::printf("bot=%s seed=%llu fills=%llu pnl=%lld %s\n",
                        res.bot_name.c_str(), (unsigned long long)res.seed,
                        (unsigned long long)res.fills,
                        (long long)res.realized_pnl_lots,
                        res.disqualified ? "DISQUALIFIED" : "");
        }
    }
    std::fprintf(out, "\n]\n");
    std::fclose(out);
    std::printf("wrote %s\n", out_path.c_str());
    return 0;
}
