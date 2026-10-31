// Second translation unit for the ask reservation host test. The slot must be
// ONE object program-wide: this TU reserves and releases it through the same
// header tiny_button.cpp and tiny_node.cpp include, and the main TU checks it
// sees the effect. With `static inline` linkage each TU had its own slot.
#include "tiny/tiny_ask_reservation.h"

bool other_tu_reserve() { return tiny_ask_reservation::reserve(); }
void other_tu_release() { tiny_ask_reservation::release(); }
bool other_tu_held() { return tiny_ask_reservation::held(); }
