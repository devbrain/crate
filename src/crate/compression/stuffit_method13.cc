#include <crate/compression/stuffit_method13.hh>
#include <algorithm>

namespace crate {

// Static Huffman tables for Method 13 (from XADMaster)
// Table 1
static const int FirstCodeLengths_1[321] = {
    4, 5, 7, 8, 8, 9, 9, 9, 9, 7, 9, 9, 9, 8, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 10, 9, 9, 10, 10, 9, 10, 9, 9,
    5, 9, 9, 9, 9, 10, 9, 9, 9, 9, 9, 9, 9, 9, 7, 9,
    9, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 8, 9, 9, 8, 8, 9, 9, 9, 9, 9, 9, 9, 7, 8, 9,
    7, 9, 9, 7, 7, 9, 9, 9, 9, 10, 9, 10, 10, 10, 9, 9,
    9, 5, 9, 8, 7, 5, 9, 8, 8, 7, 9, 9, 8, 8, 5, 5,
    7, 10, 5, 8, 5, 8, 9, 9, 9, 9, 9, 10, 9, 9, 10, 9,
    9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 9, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 9, 10, 9, 5,
    6, 5, 5, 8, 9, 9, 9, 9, 9, 9, 10, 10, 10, 9, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 9, 10, 9, 9, 9, 10, 9, 10, 9, 10, 9, 10, 9,
    10, 10, 10, 9, 10, 9, 10, 10, 9, 9, 9, 6, 9, 9, 10, 9,
    5,
};
static const int SecondCodeLengths_1[321] = {
    4, 5, 6, 6, 7, 7, 6, 7, 7, 7, 6, 8, 7, 8, 8, 8,
    8, 9, 6, 9, 8, 9, 8, 9, 9, 9, 8, 10, 5, 9, 7, 9,
    6, 9, 8, 10, 9, 10, 8, 8, 9, 9, 7, 9, 8, 9, 8, 9,
    8, 8, 6, 9, 9, 8, 8, 9, 9, 10, 8, 9, 9, 10, 8, 10,
    8, 8, 8, 8, 8, 9, 7, 10, 6, 9, 9, 11, 7, 8, 8, 9,
    8, 10, 7, 8, 6, 9, 10, 9, 9, 10, 8, 11, 9, 11, 9, 10,
    9, 8, 9, 8, 8, 8, 8, 10, 9, 9, 10, 10, 8, 9, 8, 8,
    8, 11, 9, 8, 8, 9, 9, 10, 8, 11, 10, 10, 8, 10, 9, 10,
    8, 9, 9, 11, 9, 11, 9, 10, 10, 11, 10, 12, 9, 12, 10, 11,
    10, 11, 9, 10, 10, 11, 10, 11, 10, 11, 10, 11, 10, 10, 10, 9,
    9, 9, 8, 7, 6, 8, 11, 11, 9, 12, 10, 12, 9, 11, 11, 11,
    10, 12, 11, 11, 10, 12, 10, 11, 10, 10, 10, 11, 10, 11, 11, 11,
    9, 12, 10, 12, 11, 12, 10, 11, 10, 12, 11, 12, 11, 12, 11, 12,
    10, 12, 11, 12, 11, 11, 10, 12, 10, 11, 10, 12, 10, 12, 10, 12,
    10, 11, 11, 11, 10, 11, 11, 11, 10, 12, 11, 12, 10, 10, 11, 11,
    9, 12, 11, 12, 10, 11, 10, 12, 10, 11, 10, 12, 10, 11, 10, 7,
    5, 4, 6, 6, 7, 7, 7, 8, 8, 7, 7, 6, 8, 6, 7, 7,
    9, 8, 9, 9, 10, 11, 11, 11, 12, 11, 10, 11, 12, 11, 12, 11,
    12, 12, 12, 12, 11, 12, 12, 11, 12, 11, 12, 11, 13, 11, 12, 10,
    13, 10, 14, 14, 13, 14, 15, 14, 16, 15, 15, 18, 18, 18, 9, 18,
    8,
};
static const int OffsetCodeLengths_1[11] = {5, 6, 3, 3, 3, 3, 3, 3, 3, 4, 6};

// Table 2
static const int FirstCodeLengths_2[321] = {
    4, 7, 7, 8, 7, 8, 8, 8, 8, 7, 8, 7, 8, 7, 9, 8,
    8, 8, 9, 9, 9, 9, 10, 10, 9, 10, 10, 10, 10, 10, 9, 9,
    5, 9, 8, 9, 9, 11, 10, 9, 8, 9, 9, 9, 8, 9, 7, 8,
    8, 8, 9, 9, 9, 9, 9, 10, 9, 9, 9, 10, 9, 9, 10, 9,
    8, 8, 7, 7, 7, 8, 8, 9, 8, 8, 9, 9, 8, 8, 7, 8,
    7, 10, 8, 7, 7, 9, 9, 9, 9, 10, 10, 11, 11, 11, 10, 9,
    8, 6, 8, 7, 7, 5, 7, 7, 7, 6, 9, 8, 6, 7, 6, 6,
    7, 9, 6, 6, 6, 7, 8, 8, 8, 8, 9, 10, 9, 10, 9, 9,
    8, 9, 10, 10, 9, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 11, 10, 10, 10, 10, 10, 10, 10, 11, 10, 11, 10, 10,
    9, 11, 10, 10, 10, 10, 10, 10, 9, 9, 10, 11, 10, 11, 10, 11,
    10, 12, 10, 11, 10, 12, 11, 12, 10, 12, 10, 11, 10, 11, 11, 11,
    9, 10, 11, 11, 11, 12, 12, 10, 10, 10, 11, 11, 10, 11, 10, 10,
    9, 11, 10, 11, 10, 11, 11, 11, 10, 11, 11, 12, 11, 11, 10, 10,
    10, 11, 10, 10, 11, 11, 12, 10, 10, 11, 11, 12, 11, 11, 10, 11,
    9, 12, 10, 11, 11, 11, 10, 11, 10, 11, 10, 11, 9, 10, 9, 7,
    3, 5, 6, 6, 7, 7, 8, 8, 8, 9, 9, 9, 11, 10, 10, 10,
    12, 13, 11, 12, 12, 11, 13, 12, 12, 11, 12, 12, 13, 12, 14, 13,
    14, 13, 15, 13, 14, 15, 15, 14, 13, 15, 15, 14, 15, 14, 15, 15,
    14, 15, 13, 13, 14, 15, 15, 14, 14, 16, 16, 15, 15, 15, 12, 15,
    10,
};
static const int SecondCodeLengths_2[321] = {
    5, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 8, 7, 8, 7, 7,
    7, 8, 8, 8, 8, 9, 8, 9, 8, 9, 9, 9, 7, 9, 8, 8,
    6, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 9, 8, 8,
    8, 8, 8, 9, 8, 9, 8, 9, 9, 10, 8, 10, 8, 9, 9, 8,
    8, 8, 7, 8, 8, 9, 8, 9, 7, 9, 8, 10, 8, 9, 8, 9,
    8, 9, 8, 8, 8, 9, 9, 9, 9, 10, 9, 11, 9, 10, 9, 10,
    8, 8, 8, 9, 8, 8, 8, 9, 9, 8, 9, 10, 8, 9, 8, 8,
    8, 11, 8, 7, 8, 9, 9, 9, 9, 10, 9, 10, 9, 10, 9, 8,
    8, 9, 9, 10, 9, 10, 9, 10, 8, 10, 9, 10, 9, 11, 10, 11,
    9, 11, 10, 10, 10, 11, 9, 11, 9, 10, 9, 11, 9, 11, 10, 10,
    9, 10, 9, 9, 8, 10, 9, 11, 9, 9, 9, 11, 10, 11, 9, 11,
    9, 11, 9, 11, 10, 11, 10, 11, 10, 11, 9, 10, 10, 11, 10, 10,
    8, 10, 9, 10, 10, 11, 9, 11, 9, 10, 10, 11, 9, 10, 10, 9,
    9, 10, 9, 10, 9, 10, 9, 10, 9, 11, 9, 11, 10, 10, 9, 10,
    9, 11, 9, 11, 9, 11, 9, 10, 9, 11, 9, 11, 9, 11, 9, 10,
    8, 11, 9, 10, 9, 10, 9, 10, 8, 10, 8, 9, 8, 9, 8, 7,
    4, 4, 5, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 7, 8, 8,
    9, 9, 10, 10, 10, 10, 10, 10, 11, 11, 10, 10, 12, 11, 11, 12,
    12, 11, 12, 12, 11, 12, 12, 12, 12, 12, 12, 11, 12, 11, 13, 12,
    13, 12, 13, 14, 14, 14, 15, 13, 14, 13, 14, 18, 18, 17, 7, 16,
    9,
};
static const int OffsetCodeLengths_2[13] = {5, 6, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 6};

// Table 3
static const int FirstCodeLengths_3[321] = {
    6, 6, 6, 6, 6, 9, 8, 8, 4, 9, 8, 9, 8, 9, 9, 9,
    8, 9, 9, 10, 8, 10, 10, 10, 9, 10, 10, 10, 9, 10, 10, 9,
    9, 9, 8, 10, 9, 10, 9, 10, 9, 10, 9, 10, 9, 9, 8, 9,
    8, 9, 9, 9, 10, 10, 10, 10, 9, 9, 9, 10, 9, 10, 9, 9,
    7, 8, 8, 9, 8, 9, 9, 9, 8, 9, 9, 10, 9, 9, 8, 9,
    8, 9, 8, 8, 8, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 9,
    8, 8, 9, 8, 9, 7, 8, 8, 9, 8, 10, 10, 8, 9, 8, 8,
    8, 10, 8, 8, 8, 8, 9, 9, 9, 9, 10, 10, 10, 10, 10, 9,
    7, 9, 9, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 9,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 9, 9, 9, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 9,
    8, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9,
    9, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 9, 9, 10, 9, 9, 8, 9, 8, 9,
    4, 6, 6, 6, 7, 8, 8, 9, 9, 10, 10, 10, 9, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 7, 10,
    10, 10, 7, 10, 10, 7, 7, 7, 7, 7, 6, 7, 10, 7, 7, 10,
    7, 7, 7, 6, 7, 6, 6, 7, 7, 6, 6, 9, 6, 9, 10, 6,
    10,
};
static const int SecondCodeLengths_3[321] = {
    5, 6, 6, 6, 6, 7, 7, 7, 6, 8, 7, 8, 7, 9, 8, 8,
    7, 7, 8, 9, 9, 9, 9, 10, 8, 9, 9, 10, 8, 10, 9, 8,
    6, 10, 8, 10, 8, 10, 9, 9, 9, 9, 9, 10, 9, 9, 8, 9,
    8, 9, 8, 9, 9, 10, 9, 10, 9, 9, 8, 10, 9, 11, 10, 8,
    8, 8, 8, 9, 7, 9, 9, 10, 8, 9, 8, 11, 9, 10, 9, 10,
    8, 9, 9, 9, 9, 8, 9, 9, 10, 10, 10, 12, 10, 11, 10, 10,
    8, 9, 9, 9, 8, 9, 8, 8, 10, 9, 10, 11, 8, 10, 9, 9,
    8, 12, 8, 9, 9, 9, 9, 8, 9, 10, 9, 12, 10, 10, 10, 8,
    7, 11, 10, 9, 10, 11, 9, 11, 7, 11, 10, 12, 10, 12, 10, 11,
    9, 11, 9, 12, 10, 12, 10, 12, 10, 9, 11, 12, 10, 12, 10, 11,
    9, 10, 9, 10, 9, 11, 11, 12, 9, 10, 8, 12, 11, 12, 9, 12,
    10, 12, 10, 13, 10, 12, 10, 12, 10, 12, 10, 9, 10, 12, 10, 9,
    8, 11, 10, 12, 10, 12, 10, 12, 10, 11, 10, 12, 8, 12, 10, 11,
    10, 10, 10, 12, 9, 11, 10, 12, 10, 12, 11, 12, 10, 9, 10, 12,
    9, 10, 10, 12, 10, 11, 10, 11, 10, 12, 8, 12, 9, 12, 8, 12,
    8, 11, 10, 11, 10, 11, 9, 10, 8, 10, 9, 9, 8, 9, 8, 7,
    4, 3, 5, 5, 6, 5, 6, 6, 7, 7, 8, 8, 8, 7, 7, 7,
    9, 8, 9, 9, 11, 9, 11, 9, 8, 9, 9, 11, 12, 11, 12, 12,
    13, 13, 12, 13, 14, 13, 14, 13, 14, 13, 13, 13, 12, 13, 13, 12,
    13, 13, 14, 14, 13, 13, 14, 14, 14, 14, 15, 18, 17, 18, 8, 16,
    10,
};
static const int OffsetCodeLengths_3[14] = {6, 7, 4, 4, 3, 3, 3, 3, 3, 4, 4, 4, 5, 7};

// Table 4
static const int FirstCodeLengths_4[321] = {
    2, 6, 6, 7, 7, 8, 7, 8, 7, 8, 8, 9, 8, 9, 9, 9,
    8, 8, 9, 9, 9, 10, 10, 9, 8, 10, 9, 10, 9, 10, 9, 9,
    6, 9, 8, 9, 9, 10, 9, 9, 9, 10, 9, 9, 9, 9, 8, 8,
    8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 9,
    7, 7, 8, 8, 8, 8, 9, 9, 7, 8, 9, 10, 8, 8, 7, 8,
    8, 10, 8, 8, 8, 9, 8, 9, 9, 10, 9, 11, 10, 11, 9, 9,
    8, 7, 9, 8, 8, 6, 8, 8, 8, 7, 10, 9, 7, 8, 7, 7,
    8, 10, 7, 7, 7, 8, 9, 9, 9, 9, 10, 11, 9, 11, 10, 9,
    7, 9, 10, 10, 10, 11, 11, 10, 10, 11, 10, 10, 10, 11, 11, 10,
    9, 10, 10, 11, 10, 11, 10, 11, 10, 10, 10, 11, 10, 11, 10, 10,
    9, 10, 10, 11, 10, 10, 10, 10, 9, 10, 10, 10, 10, 11, 10, 11,
    10, 11, 10, 11, 11, 11, 10, 12, 10, 11, 10, 11, 10, 11, 11, 10,
    8, 10, 10, 11, 10, 11, 11, 11, 10, 11, 10, 11, 10, 11, 11, 11,
    9, 10, 11, 11, 10, 11, 11, 11, 10, 11, 11, 11, 10, 10, 10, 10,
    10, 11, 10, 10, 11, 11, 10, 10, 9, 11, 10, 10, 11, 11, 10, 10,
    10, 11, 10, 10, 10, 10, 10, 10, 9, 11, 10, 10, 8, 10, 8, 6,
    5, 6, 6, 7, 7, 8, 8, 8, 9, 10, 11, 10, 10, 11, 11, 12,
    12, 10, 11, 12, 12, 12, 12, 13, 13, 13, 13, 13, 12, 13, 13, 15,
    14, 12, 14, 15, 16, 12, 12, 13, 15, 14, 16, 15, 17, 18, 15, 17,
    16, 15, 15, 15, 15, 13, 13, 10, 14, 12, 13, 17, 17, 18, 10, 17,
    4,
};
static const int SecondCodeLengths_4[321] = {
    4, 5, 6, 6, 6, 6, 7, 7, 6, 7, 7, 9, 6, 8, 8, 7,
    7, 8, 8, 8, 6, 9, 8, 8, 7, 9, 8, 9, 8, 9, 8, 9,
    6, 9, 8, 9, 8, 10, 9, 9, 8, 10, 8, 10, 8, 9, 8, 9,
    8, 8, 7, 9, 9, 9, 9, 9, 8, 10, 9, 10, 9, 10, 9, 8,
    7, 8, 9, 9, 8, 9, 9, 9, 7, 10, 9, 10, 9, 9, 8, 9,
    8, 9, 8, 8, 8, 9, 9, 10, 9, 9, 8, 11, 9, 11, 10, 10,
    8, 8, 10, 8, 8, 9, 9, 9, 10, 9, 10, 11, 9, 9, 9, 9,
    8, 9, 8, 8, 8, 10, 10, 9, 9, 8, 10, 11, 10, 11, 11, 9,
    8, 9, 10, 11, 9, 10, 11, 11, 9, 12, 10, 10, 10, 12, 11, 11,
    9, 11, 11, 12, 9, 11, 9, 10, 10, 10, 10, 12, 9, 11, 10, 11,
    9, 11, 11, 11, 10, 11, 11, 12, 9, 10, 10, 12, 11, 11, 10, 11,
    9, 11, 10, 11, 10, 11, 9, 11, 11, 9, 8, 11, 10, 11, 11, 10,
    7, 12, 11, 11, 11, 11, 11, 12, 10, 12, 11, 13, 11, 10, 12, 11,
    10, 11, 10, 11, 10, 11, 11, 11, 10, 12, 11, 11, 10, 11, 10, 10,
    10, 11, 10, 12, 11, 12, 10, 11, 9, 11, 10, 11, 10, 11, 10, 12,
    9, 11, 11, 11, 9, 11, 10, 10, 9, 11, 10, 10, 9, 10, 9, 7,
    4, 5, 5, 5, 6, 6, 7, 6, 8, 7, 8, 9, 9, 7, 8, 8,
    10, 9, 10, 10, 12, 10, 11, 11, 11, 11, 10, 11, 12, 11, 11, 11,
    11, 11, 13, 12, 11, 12, 13, 12, 12, 12, 13, 11, 9, 12, 13, 7,
    13, 11, 13, 11, 10, 11, 13, 15, 15, 12, 14, 15, 15, 15, 6, 15,
    5,
};
static const int OffsetCodeLengths_4[11] = {3, 6, 5, 4, 2, 3, 3, 3, 4, 4, 6};

// Table 5
static const int FirstCodeLengths_5[321] = {
    7, 9, 9, 9, 9, 9, 9, 9, 9, 8, 9, 9, 9, 7, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 10, 9, 10, 9, 10, 9, 10, 9, 9,
    5, 9, 7, 9, 9, 9, 9, 9, 7, 7, 7, 9, 7, 7, 8, 7,
    8, 8, 7, 7, 9, 9, 9, 9, 7, 7, 7, 9, 9, 9, 9, 9,
    9, 7, 9, 7, 7, 7, 7, 9, 9, 7, 9, 9, 7, 7, 7, 7,
    7, 9, 7, 8, 7, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 7, 8, 7, 7, 7, 8, 8, 6, 7, 9, 7, 7, 8, 7, 5,
    6, 9, 5, 7, 5, 6, 7, 7, 9, 8, 9, 9, 9, 9, 9, 9,
    9, 9, 10, 9, 10, 10, 10, 9, 9, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 10,
    9, 10, 10, 10, 9, 9, 10, 9, 9, 9, 9, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 9, 10, 10, 10, 9, 9, 9, 10, 10, 10, 10, 10,
    9, 10, 9, 10, 10, 9, 10, 10, 9, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    9, 10, 10, 10, 10, 10, 10, 10, 9, 10, 9, 10, 9, 10, 10, 9,
    5, 6, 8, 8, 7, 7, 7, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 9, 10, 10, 5, 10, 8, 9, 8,
    9,
};
static const int SecondCodeLengths_5[321] = {
    8, 10, 11, 11, 11, 12, 11, 11, 12, 6, 11, 12, 10, 5, 12, 12,
    12, 12, 12, 12, 12, 13, 13, 14, 13, 13, 12, 13, 12, 13, 12, 15,
    4, 10, 7, 9, 11, 11, 10, 9, 6, 7, 8, 9, 6, 7, 6, 7,
    8, 7, 7, 8, 8, 8, 8, 8, 8, 9, 8, 7, 10, 9, 10, 10,
    11, 7, 8, 6, 7, 8, 8, 9, 8, 7, 10, 10, 8, 7, 8, 8,
    7, 10, 7, 6, 7, 9, 9, 8, 11, 11, 11, 10, 11, 11, 11, 8,
    11, 6, 7, 6, 6, 6, 6, 8, 7, 6, 10, 9, 6, 7, 6, 6,
    7, 10, 6, 5, 6, 7, 7, 7, 10, 8, 11, 9, 13, 7, 14, 16,
    12, 14, 14, 15, 15, 16, 16, 14, 15, 15, 15, 15, 15, 15, 15, 15,
    14, 15, 13, 14, 14, 16, 15, 17, 14, 17, 15, 17, 12, 14, 13, 16,
    12, 17, 13, 17, 14, 13, 13, 14, 14, 12, 13, 15, 15, 14, 15, 17,
    14, 17, 15, 14, 15, 16, 12, 16, 15, 14, 15, 16, 15, 16, 17, 17,
    15, 15, 17, 17, 13, 14, 15, 15, 13, 12, 16, 16, 17, 14, 15, 16,
    15, 15, 13, 13, 15, 13, 16, 17, 15, 17, 17, 17, 16, 17, 14, 17,
    14, 16, 15, 17, 15, 15, 14, 17, 15, 17, 15, 16, 15, 15, 16, 16,
    14, 17, 17, 15, 15, 16, 15, 17, 15, 14, 16, 16, 16, 16, 16, 12,
    4, 4, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 8, 9, 9,
    9, 9, 9, 10, 10, 10, 11, 10, 11, 11, 11, 11, 11, 12, 12, 12,
    13, 13, 12, 13, 12, 14, 14, 12, 13, 13, 13, 13, 14, 12, 13, 13,
    14, 14, 14, 13, 14, 14, 15, 15, 13, 15, 13, 17, 17, 17, 9, 17,
    7,
};
static const int OffsetCodeLengths_5[11] = {6, 7, 7, 6, 4, 3, 2, 2, 3, 3, 6};

// Table pointers
static const int* FirstCodeLengths[5] = {
    FirstCodeLengths_1, FirstCodeLengths_2, FirstCodeLengths_3,
    FirstCodeLengths_4, FirstCodeLengths_5
};
static const int* SecondCodeLengths[5] = {
    SecondCodeLengths_1, SecondCodeLengths_2, SecondCodeLengths_3,
    SecondCodeLengths_4, SecondCodeLengths_5
};
static const int* OffsetCodeLengths[5] = {
    OffsetCodeLengths_1, OffsetCodeLengths_2, OffsetCodeLengths_3,
    OffsetCodeLengths_4, OffsetCodeLengths_5
};
static const int OffsetCodeSize[5] = {11, 13, 14, 11, 11};

// Meta codes for dynamic table building
static const int MetaCodes[37] = {
    0x5d8, 0x058, 0x040, 0x0c0, 0x000, 0x078, 0x02b, 0x014,
    0x00c, 0x01c, 0x01b, 0x00b, 0x010, 0x020, 0x038, 0x018,
    0x0d8, 0xbd8, 0x180, 0x680, 0x380, 0xf80, 0x780, 0x480,
    0x080, 0x280, 0x3d8, 0xfd8, 0x7d8, 0x9d8, 0x1d8, 0x004,
    0x001, 0x002, 0x007, 0x003, 0x008
};

static const int MetaCodeLengths[37] = {
    11, 8, 8, 8, 8, 7, 6, 5, 5, 5, 5, 6, 5, 6, 7, 7, 9, 12, 10, 11, 11, 12,
    12, 11, 11, 11, 12, 12, 12, 12, 12, 5, 2, 2, 3, 4, 5
};

// ============================================================================
// huffman_table implementation - matching original stuffit.cc approach
// ============================================================================

stuffit_method13_decompressor::huffman_table::huffman_table() {
    clear();
}

void stuffit_method13_decompressor::huffman_table::clear() {
    tree_.assign(MAX_SYMBOLS * 2, 0);
    num_symbols_ = 0;
    next_free_ = 2;
}

void stuffit_method13_decompressor::huffman_table::add_code(u32 code, int len, int symbol) {
    size_t node = 0;
    for (int i = 0; i < len - 1; i++) {
        size_t bit = (code >> i) & 1;
        if (tree_[node + bit] == 0) {
            tree_[node + bit] = next_free_;
            next_free_ += 2;
        }
        node = static_cast<size_t>(tree_[node + bit]);
    }
    size_t bit = (code >> (len - 1)) & 1;
    tree_[node + bit] = static_cast<int>(num_symbols_) * 2 + symbol;
}

void stuffit_method13_decompressor::huffman_table::init_from_lengths(
    const int* lengths, size_t num_symbols, bool lsb_first) {

    clear();
    num_symbols_ = num_symbols;

    // Count codes per length
    std::array<size_t, 32> bl_count{};
    size_t max_len = 0;
    for (size_t i = 0; i < num_symbols; i++) {
        if (lengths[i] > 0) {
            bl_count[static_cast<size_t>(lengths[i])]++;
            if (static_cast<size_t>(lengths[i]) > max_len) {
                max_len = static_cast<size_t>(lengths[i]);
            }
        }
    }

    // Generate starting code for each length
    std::array<u32, 32> next_code{};
    u32 code = 0;
    for (size_t bits = 1; bits <= max_len; bits++) {
        code = (code + static_cast<u32>(bl_count[bits - 1])) << 1;
        next_code[bits] = code;
    }

    // Assign codes to symbols
    for (size_t i = 0; i < num_symbols; i++) {
        int len = lengths[i];
        if (len > 0) {
            u32 c = next_code[static_cast<size_t>(len)]++;
            // Reverse bits if LSB first
            if (lsb_first) {
                u32 rev = 0;
                for (int b = 0; b < len; b++) {
                    rev = (rev << 1) | ((c >> b) & 1);
                }
                c = rev;
            }
            add_code(c, len, static_cast<int>(i));
        }
    }
}

void stuffit_method13_decompressor::huffman_table::init_from_explicit_codes(
    const int* codes, const int* lengths, size_t num_symbols) {

    clear();
    num_symbols_ = num_symbols;

    for (size_t i = 0; i < num_symbols; i++) {
        if (lengths[i] > 0) {
            add_code(static_cast<u32>(codes[i]), lengths[i], static_cast<int>(i));
        }
    }
}

template<typename Reader>
int stuffit_method13_decompressor::huffman_table::decode(Reader& reader) const {
    size_t node = 0;
    while (node < num_symbols_ * 2) {
        int bit = reader.read_bit();
        if (bit < 0) return -1;  // Need more input
        node = static_cast<size_t>(tree_[node + static_cast<size_t>(bit)]);
        if (node >= num_symbols_ * 2) {
            return static_cast<int>(node - num_symbols_ * 2);
        }
        if (node == 0) return -2;  // Invalid code
    }
    return -2;
}

// ============================================================================
// Bit buffer operations
// ============================================================================

bool stuffit_method13_decompressor::try_peek_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    while (bits_left_ < n) {
        if (ptr >= end) return false;
        bit_buffer_ |= static_cast<u32>(*ptr++) << bits_left_;
        bits_left_ += 8;
    }
    out = bit_buffer_ & ((1u << n) - 1);
    return true;
}

bool stuffit_method13_decompressor::try_read_bits(const byte*& ptr, const byte* end, unsigned n, u32& out) {
    if (!try_peek_bits(ptr, end, n, out)) return false;
    remove_bits(n);
    return true;
}

void stuffit_method13_decompressor::remove_bits(unsigned n) {
    bit_buffer_ >>= n;
    bits_left_ -= n;
}

// ============================================================================
// Dynamic table parsing - matching original XADMaster algorithm
// ============================================================================

int stuffit_method13_decompressor::try_parse_dynamic_table(
    const byte*& ptr, const byte* end,
    huffman_table& table, size_t num_codes) {

    if (dyn_state_.lengths.empty()) {
        dyn_state_.lengths.resize(num_codes, 0);
        dyn_state_.index = 0;
        dyn_state_.num_codes = num_codes;
        dyn_state_.length = 0;
        dyn_state_.meta_value = -1;
        dyn_state_.repeat_count = 0;
        dyn_state_.reading_extra = false;
    }

    // Bit reader helper for Huffman decoding
    struct bit_reader {
        stuffit_method13_decompressor* dec;
        const byte** ptr;
        const byte* end;

        int read_bit() {
            u32 bit;
            if (!dec->try_read_bits(*ptr, end, 1, bit)) return -1;
            return static_cast<int>(bit);
        }
    };
    bit_reader br{this, &ptr, end};

    while (dyn_state_.index < dyn_state_.num_codes) {
        // If we have pending repeats, handle them
        if (dyn_state_.repeat_count > 0) {
            dyn_state_.lengths[dyn_state_.index] = dyn_state_.length;
            dyn_state_.index++;
            dyn_state_.repeat_count--;
            continue;
        }

        // If we're waiting for extra bits
        if (dyn_state_.reading_extra) {
            u32 extra;
            if (!try_read_bits(ptr, end, static_cast<unsigned>(dyn_state_.extra_bits_needed), extra)) {
                return 0;  // Need more input
            }

            // Case 34: conditional advance
            if (dyn_state_.meta_value == 34) {
                if (extra) {
                    if (dyn_state_.index < dyn_state_.num_codes) {
                        dyn_state_.lengths[dyn_state_.index++] = dyn_state_.length;
                    }
                }
            }
            // Case 35: repeat 2-9 times
            else if (dyn_state_.meta_value == 35) {
                dyn_state_.repeat_count = static_cast<int>(extra) + 2;
            }
            // Case 36: repeat 10-73 times
            else if (dyn_state_.meta_value == 36) {
                dyn_state_.repeat_count = static_cast<int>(extra) + 10;
            }

            dyn_state_.reading_extra = false;
            dyn_state_.meta_value = -1;

            // If we handled the case, continue
            if (dyn_state_.repeat_count > 0) continue;

            // Set current symbol's length (end of loop iteration)
            if (dyn_state_.index < dyn_state_.num_codes) {
                dyn_state_.lengths[dyn_state_.index] = dyn_state_.length;
            }
            dyn_state_.index++;
            continue;
        }

        // Decode meta symbol
        int meta = meta_code_.decode(br);
        if (meta < 0) {
            if (meta == -1) return 0;  // Need more input
            return -1;  // Error
        }

        switch (meta) {
            case 31:  // Set length to -1 (invalid/unused)
                dyn_state_.length = -1;
                break;
            case 32:  // Increment length
                dyn_state_.length++;
                break;
            case 33:  // Decrement length
                dyn_state_.length--;
                break;
            case 34:  // Conditional: if next bit is 1, set and advance
                dyn_state_.meta_value = 34;
                dyn_state_.extra_bits_needed = 1;
                dyn_state_.reading_extra = true;
                continue;  // Don't set length yet
            case 35:  // Repeat 2-9 times
                dyn_state_.meta_value = 35;
                dyn_state_.extra_bits_needed = 3;
                dyn_state_.reading_extra = true;
                continue;  // Don't set length yet
            case 36:  // Repeat 10-73 times
                dyn_state_.meta_value = 36;
                dyn_state_.extra_bits_needed = 6;
                dyn_state_.reading_extra = true;
                continue;  // Don't set length yet
            default:  // 0-30: Set length = val + 1
                dyn_state_.length = meta + 1;
                break;
        }

        // Set current symbol's length
        if (dyn_state_.index < dyn_state_.num_codes) {
            dyn_state_.lengths[dyn_state_.index] = dyn_state_.length;
        }
        dyn_state_.index++;
    }

    // Complete - build the table
    table.init_from_lengths(dyn_state_.lengths.data(), dyn_state_.num_codes, true);
    dyn_state_.lengths.clear();
    return 1;  // Done
}

// ============================================================================
// Main decompressor
// ============================================================================

stuffit_method13_decompressor::stuffit_method13_decompressor() {
    reset();
}

void stuffit_method13_decompressor::reset() {
    state_ = state::READ_HEADER;
    bit_buffer_ = 0;
    bits_left_ = 0;
    header_byte_ = 0;
    code_type_ = 0;
    reuse_first_as_second_ = false;
    offset_code_size_ = 0;
    use_first_code_ = true;

    meta_code_.clear();
    first_code_.clear();
    second_code_.clear();
    offset_code_.clear();

    dyn_state_ = {};

    window_.assign(WINDOW_SIZE, byte{0});
    window_pos_ = 0;

    match_length_ = 0;
    match_offset_ = 0;
    match_copied_ = 0;
    offset_bits_ = 0;
    decode_node_ = 0;
}

result_t<stream_result> stuffit_method13_decompressor::decompress_some(
    byte_span input,
    mutable_byte_span output,
    bool input_finished
) {
    const byte* in_ptr = input.data();
    const byte* in_end = input.data() + input.size();
    byte* out_ptr = output.data();
    byte* out_end = output.data() + output.size();

    auto bytes_read = [&]() -> size_t {
        return static_cast<size_t>(in_ptr - input.data());
    };
    auto bytes_written = [&]() -> size_t {
        return static_cast<size_t>(out_ptr - output.data());
    };

    // Bit reader helper for Huffman decoding
    struct bit_reader {
        stuffit_method13_decompressor* dec;
        const byte** ptr;
        const byte* end;

        int read_bit() {
            u32 bit;
            if (!dec->try_read_bits(*ptr, end, 1, bit)) return -1;
            return static_cast<int>(bit);
        }
    };
    bit_reader br{this, &in_ptr, in_end};

    while (state_ != state::DONE) {
        switch (state_) {
            case state::READ_HEADER: {
                if (in_ptr >= in_end) {
                    if (input_finished) {
                        state_ = state::DONE;
                        return stream_result::done(bytes_read(), bytes_written());
                    }
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                header_byte_ = static_cast<u8>(*in_ptr++);
                code_type_ = header_byte_ >> 4;
                reuse_first_as_second_ = (header_byte_ & 0x08) != 0;
                offset_code_size_ = static_cast<size_t>((header_byte_ & 0x07) + 10);

                if (code_type_ == 0) {
                    state_ = state::BUILD_META_TABLE;
                } else if (code_type_ >= 1 && code_type_ <= 5) {
                    state_ = state::BUILD_STATIC_TABLES;
                } else {
                    return crate::make_unexpected(error{error_code::CorruptData, "Invalid method 13 code type"});
                }
                break;
            }

            case state::BUILD_STATIC_TABLES: {
                size_t table_idx = static_cast<size_t>(code_type_ - 1);
                first_code_.init_from_lengths(FirstCodeLengths[table_idx], 321, true);
                second_code_.init_from_lengths(SecondCodeLengths[table_idx], 321, true);
                offset_code_.init_from_lengths(OffsetCodeLengths[table_idx],
                                                static_cast<size_t>(OffsetCodeSize[table_idx]), true);
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::BUILD_META_TABLE: {
                meta_code_.init_from_explicit_codes(MetaCodes, MetaCodeLengths, 37);
                state_ = state::PARSE_FIRST_CODE;
                break;
            }

            case state::PARSE_FIRST_CODE: {
                int result = try_parse_dynamic_table(in_ptr, in_end, first_code_, 321);
                if (result == 0) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                if (result < 0) {
                    return crate::make_unexpected(error{error_code::CorruptData, "Failed to parse first code table"});
                }
                state_ = state::PARSE_SECOND_CODE;
                break;
            }

            case state::PARSE_SECOND_CODE: {
                if (reuse_first_as_second_) {
                    second_code_ = first_code_;
                    state_ = state::PARSE_OFFSET_CODE;
                } else {
                    int result = try_parse_dynamic_table(in_ptr, in_end, second_code_, 321);
                    if (result == 0) {
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    if (result < 0) {
                        return crate::make_unexpected(error{error_code::CorruptData, "Failed to parse second code table"});
                    }
                    state_ = state::PARSE_OFFSET_CODE;
                }
                break;
            }

            case state::PARSE_OFFSET_CODE: {
                int result = try_parse_dynamic_table(in_ptr, in_end, offset_code_, offset_code_size_);
                if (result == 0) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                if (result < 0) {
                    return crate::make_unexpected(error{error_code::CorruptData, "Failed to parse offset code table"});
                }
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::DECODE_SYMBOL: {
                huffman_table& curr_code = use_first_code_ ? first_code_ : second_code_;
                int sym = curr_code.decode(br);
                if (sym < 0) {
                    if (sym == -1) {
                        if (input_finished) {
                            state_ = state::DONE;
                            return stream_result::done(bytes_read(), bytes_written());
                        }
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    return crate::make_unexpected(error{error_code::CorruptData, "Invalid Huffman code"});
                }

                if (sym < 0x100) {
                    // Literal byte
                    if (out_ptr >= out_end) {
                        // Need to re-decode this symbol later - save state
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    byte b = static_cast<byte>(sym);
                    *out_ptr++ = b;
                    window_[window_pos_++ % WINDOW_SIZE] = b;
                    use_first_code_ = true;
                } else if (sym < 0x140) {
                    // Match
                    use_first_code_ = false;
                    if (sym < 0x13e) {
                        match_length_ = static_cast<size_t>(sym - 0x100 + 3);
                        state_ = state::DECODE_OFFSET;
                    } else if (sym == 0x13e) {
                        state_ = state::READ_MATCH_LENGTH_10;
                    } else if (sym == 0x13f) {
                        state_ = state::READ_MATCH_LENGTH_15;
                    }
                } else {
                    // End of stream (0x140)
                    state_ = state::DONE;
                }
                break;
            }

            case state::READ_MATCH_LENGTH_10: {
                u32 len;
                if (!try_read_bits(in_ptr, in_end, 10, len)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                match_length_ = static_cast<size_t>(len) + 65;
                state_ = state::DECODE_OFFSET;
                break;
            }

            case state::READ_MATCH_LENGTH_15: {
                u32 len;
                if (!try_read_bits(in_ptr, in_end, 15, len)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                match_length_ = static_cast<size_t>(len) + 65;
                state_ = state::DECODE_OFFSET;
                break;
            }

            case state::DECODE_OFFSET: {
                int bit_len = offset_code_.decode(br);
                if (bit_len < 0) {
                    if (bit_len == -1) {
                        return stream_result::need_input(bytes_read(), bytes_written());
                    }
                    return crate::make_unexpected(error{error_code::CorruptData, "Invalid offset code"});
                }

                if (bit_len == 0) {
                    match_offset_ = 1;
                    match_copied_ = 0;
                    state_ = state::COPY_MATCH;
                } else if (bit_len == 1) {
                    match_offset_ = 2;
                    match_copied_ = 0;
                    state_ = state::COPY_MATCH;
                } else {
                    offset_bits_ = bit_len - 1;
                    state_ = state::READ_OFFSET_EXTRA;
                }
                break;
            }

            case state::READ_OFFSET_EXTRA: {
                u32 extra;
                if (!try_read_bits(in_ptr, in_end, static_cast<unsigned>(offset_bits_), extra)) {
                    return stream_result::need_input(bytes_read(), bytes_written());
                }
                match_offset_ = (1u << offset_bits_) + extra + 1;
                match_copied_ = 0;
                state_ = state::COPY_MATCH;
                break;
            }

            case state::COPY_MATCH: {
                while (match_copied_ < match_length_) {
                    if (out_ptr >= out_end) {
                        return stream_result::need_output(bytes_read(), bytes_written());
                    }
                    size_t src_pos = (window_pos_ - match_offset_ + WINDOW_SIZE) % WINDOW_SIZE;
                    byte b = window_[src_pos];
                    *out_ptr++ = b;
                    window_[window_pos_++ % WINDOW_SIZE] = b;
                    match_copied_++;
                }
                state_ = state::DECODE_SYMBOL;
                break;
            }

            case state::DONE:
                break;
        }
    }

    return stream_result::done(bytes_read(), bytes_written());
}

}  // namespace crate
