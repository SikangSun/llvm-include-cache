#include "d.h"
#include "c.h"
int x4(struct c_rec *r) { enum c_e e = C_A; return d_use(r) + c_f(1) + e; }
