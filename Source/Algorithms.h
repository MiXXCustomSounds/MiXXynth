#pragma once

#include <algorithm>
#include <array>
#include <initializer_list>
#include <utility>

// M2a: DX7 algorithm table + free-routing matrix support.
//
// PROVENANCE: the 32 topologies below are the original Yamaha DX7 algorithms,
// encoded from the explicit textual listing published by Sean Bolton (hexter)
// on the linux-audio-user list, cross-checked against the DX7 algorithm chart.
// Algorithm topologies are factual hardware descriptions, re-encoded here in
// MiXXynth's own data format (MIT).
//
// Operator numbering matches DX7: OP1..OP6, where OP1 is index 0.
// The matrix is mod[src][dst]: "src modulates dst". Self-cells are unused
// (self-modulation is the per-op feedback path instead).
namespace mixxynth
{
    constexpr int kNumOps = 6;
    constexpr int kNumAlgorithms = 32;
    constexpr int kFreeAlgorithm = 32; // index 32 = user matrix

    struct AlgoDef
    {
        const char* name = "";
        std::array<std::array<float, kNumOps>, kNumOps> mod {}; // mod[src][dst]
        std::array<bool, kNumOps> carrier {};
        int feedbackOp = -1; // DX7's designated feedback operator, 0-based
    };

    // Build an algorithm from 1-based DX7 operator numbers (matches the chart).
    inline AlgoDef makeAlgo (const char* name,
                             std::initializer_list<std::pair<int, int>> edges,
                             std::initializer_list<int> carriers,
                             int feedbackOp)
    {
        AlgoDef a;
        a.name = name;
        a.feedbackOp = feedbackOp - 1;

        for (const auto& e : edges)
            a.mod[static_cast<size_t> (e.first - 1)][static_cast<size_t> (e.second - 1)] = 1.0f;

        for (const auto c : carriers)
            a.carrier[static_cast<size_t> (c - 1)] = true;

        return a;
    }

    // The 32 DX7 algorithms. Edges are 1-based "modulator -> carrier".
    inline const std::array<AlgoDef, kNumAlgorithms>& algorithmTable()
    {
        static const std::array<AlgoDef, kNumAlgorithms> table = { {
            // 1: 6>5>4>3, 2>1                     carriers 3,1        fb 6
            makeAlgo ("1",  { { 6, 5 }, { 5, 4 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      6),
            // 2: 6>5>4>3, 2>1                     carriers 3,1        fb 2
            makeAlgo ("2",  { { 6, 5 }, { 5, 4 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      2),
            // 3: 6>5>4, 3>2>1                     carriers 4,1        fb 6
            makeAlgo ("3",  { { 6, 5 }, { 5, 4 }, { 3, 2 }, { 2, 1 } },                { 4, 1 },      6),
            // 4: 6>5>4, 3>2>1                     carriers 4,1        fb 4
            makeAlgo ("4",  { { 6, 5 }, { 5, 4 }, { 3, 2 }, { 2, 1 } },                { 4, 1 },      4),
            // 5: 6>5, 4>3, 2>1 (three pairs)      carriers 5,3,1      fb 6
            makeAlgo ("5",  { { 6, 5 }, { 4, 3 }, { 2, 1 } },                          { 5, 3, 1 },   6),
            // 6: same topology as 5                                   fb 5
            makeAlgo ("6",  { { 6, 5 }, { 4, 3 }, { 2, 1 } },                          { 5, 3, 1 },   5),
            // 7: 6>5>3, 4>3, 2>1                  carriers 3,1        fb 6
            makeAlgo ("7",  { { 6, 5 }, { 5, 3 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      6),
            // 8: same topology as 7                                   fb 4
            makeAlgo ("8",  { { 6, 5 }, { 5, 3 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      4),
            // 9: same topology as 7                                   fb 2
            makeAlgo ("9",  { { 6, 5 }, { 5, 3 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      2),
            // 10: 6>4, 5>4, 3>2>1                 carriers 4,1        fb 3
            makeAlgo ("10", { { 6, 4 }, { 5, 4 }, { 3, 2 }, { 2, 1 } },                { 4, 1 },      3),
            // 11: same topology as 10                                 fb 6
            makeAlgo ("11", { { 6, 4 }, { 5, 4 }, { 3, 2 }, { 2, 1 } },                { 4, 1 },      6),
            // 12: 6>3, 5>3, 4>3, 2>1              carriers 3,1        fb 2
            makeAlgo ("12", { { 6, 3 }, { 5, 3 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      2),
            // 13: same topology as 12                                 fb 6
            makeAlgo ("13", { { 6, 3 }, { 5, 3 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      6),
            // 14: 6>4, 5>4, 4>3, 2>1              carriers 3,1        fb 6
            makeAlgo ("14", { { 6, 4 }, { 5, 4 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      6),
            // 15: same topology as 14                                 fb 2
            makeAlgo ("15", { { 6, 4 }, { 5, 4 }, { 4, 3 }, { 2, 1 } },                { 3, 1 },      2),
            // 16: 6>5>1, 4>3>1, 2>1               carrier 1           fb 6
            makeAlgo ("16", { { 6, 5 }, { 5, 1 }, { 4, 3 }, { 3, 1 }, { 2, 1 } },      { 1 },         6),
            // 17: 6>5>1, 4>3>1, 2>1               carrier 1           fb 2
            makeAlgo ("17", { { 6, 5 }, { 5, 1 }, { 4, 3 }, { 3, 1 }, { 2, 1 } },      { 1 },         2),
            // 18: 6>5>4>1, 3>1, 2>1               carrier 1           fb 3
            makeAlgo ("18", { { 6, 5 }, { 5, 4 }, { 4, 1 }, { 3, 1 }, { 2, 1 } },      { 1 },         3),
            // 19: 6>5, 6>4, 3>2>1                 carriers 5,4,1      fb 6
            makeAlgo ("19", { { 6, 5 }, { 6, 4 }, { 3, 2 }, { 2, 1 } },                { 5, 4, 1 },   6),
            // 20: 3>2, 3>1, 6>4, 5>4              carriers 4,2,1      fb 3
            makeAlgo ("20", { { 3, 2 }, { 3, 1 }, { 6, 4 }, { 5, 4 } },                { 4, 2, 1 },   3),
            // 21: 6>5, 6>4, 3>2, 3>1              carriers 5,4,2,1    fb 3
            makeAlgo ("21", { { 6, 5 }, { 6, 4 }, { 3, 2 }, { 3, 1 } },                { 5, 4, 2, 1 }, 3),
            // 22: 6>5, 6>4, 6>3, 2>1              carriers 5,4,3,1    fb 6
            makeAlgo ("22", { { 6, 5 }, { 6, 4 }, { 6, 3 }, { 2, 1 } },                { 5, 4, 3, 1 }, 6),
            // 23: 6>5, 6>4, 3>2                   carriers 5,4,2,1    fb 6
            makeAlgo ("23", { { 6, 5 }, { 6, 4 }, { 3, 2 } },                          { 5, 4, 2, 1 }, 6),
            // 24: 6>5, 6>4, 6>3                   carriers 5,4,3,2,1  fb 6
            makeAlgo ("24", { { 6, 5 }, { 6, 4 }, { 6, 3 } },                          { 5, 4, 3, 2, 1 }, 6),
            // 25: 6>5, 6>4                        carriers 5,4,3,2,1  fb 6
            makeAlgo ("25", { { 6, 5 }, { 6, 4 } },                                    { 5, 4, 3, 2, 1 }, 6),
            // 26: 6>4, 5>4, 3>2                   carriers 4,2,1      fb 6
            makeAlgo ("26", { { 6, 4 }, { 5, 4 }, { 3, 2 } },                          { 4, 2, 1 },   6),
            // 27: 6>4, 5>4, 3>2                   carriers 4,2,1      fb 3
            makeAlgo ("27", { { 6, 4 }, { 5, 4 }, { 3, 2 } },                          { 4, 2, 1 },   3),
            // 28: 5>4>3, 2>1, 6                   carriers 6,3,1      fb 5
            makeAlgo ("28", { { 5, 4 }, { 4, 3 }, { 2, 1 } },                          { 6, 3, 1 },   5),
            // 29: 6>5, 4>3                        carriers 5,3,2,1    fb 6
            makeAlgo ("29", { { 6, 5 }, { 4, 3 } },                                    { 5, 3, 2, 1 }, 6),
            // 30: 5>4>3, 6                        carriers 6,3,2,1    fb 5
            makeAlgo ("30", { { 5, 4 }, { 4, 3 } },                                    { 6, 3, 2, 1 }, 5),
            // 31: 6>5                             carriers 5,4,3,2,1  fb 6
            makeAlgo ("31", { { 6, 5 } },                                              { 5, 4, 3, 2, 1 }, 6),
            // 32: all six are carriers (additive)                     fb 6
            makeAlgo ("32", {},                                                        { 6, 5, 4, 3, 2, 1 }, 6)
        } };

        return table;
    }
}
