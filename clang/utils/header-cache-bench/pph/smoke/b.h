#ifndef B_H
#define B_H
typedef struct b_pair { int x, y; } b_t;
#define B_SCALE 3
static inline int b_sum(const b_t *p) { return (p->x + p->y) * B_SCALE; }
extern int b_count;
#endif
