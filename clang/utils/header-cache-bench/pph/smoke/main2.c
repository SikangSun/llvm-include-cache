#include "b.h"
#include "a.h"
int b_count = 7;
int twice(const struct a_node *n) { return 2 * a_first(n) + B_SCALE; }
