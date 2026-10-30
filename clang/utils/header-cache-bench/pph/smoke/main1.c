#include "a.h"
int a_total(const struct a_node *n) {
  int t = 0;
  for (; n; n = n->next) t = A_MAX(t, b_sum(&n->pair));
  return t + a_first(n) + b_count;
}
