#ifndef A_H
#define A_H
#include "b.h"
#define A_MAX(x, y) ((x) > (y) ? (x) : (y))
struct a_node { b_t pair; struct a_node *next; };
int a_total(const struct a_node *n);
static inline int a_first(const struct a_node *n) { return n ? b_sum(&n->pair) : 0; }
#endif
