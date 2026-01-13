#include <crate/compression/quantum.hh>

namespace crate {

void quantum_model::init(u16 start, unsigned len) {
    entries = len;
    for (unsigned i = 0; i <= len; i++) {
        symbols[i].symbol = start + static_cast<u16>(i);
        symbols[i].cumfreq = static_cast<u16>(len - i);
    }
}
void quantum_model::update(unsigned i) {
    // Increase frequencies of symbols before position i
    for (unsigned j = 0; j < i; j++) {
        symbols[j].cumfreq += 8;
    }

    // Scale down if too large
    if (symbols[0].cumfreq > 3800) {
        if (--shifts_left == 0) {
            for (unsigned j = entries; j > 0; j--) {
                symbols[j - 1].cumfreq >>= 1;
                if (symbols[j - 1].cumfreq <= symbols[j].cumfreq) {
                    symbols[j - 1].cumfreq = symbols[j].cumfreq + 1;
                }
            }
            shifts_left = 50;
        }
    }
}
}  // namespace crate