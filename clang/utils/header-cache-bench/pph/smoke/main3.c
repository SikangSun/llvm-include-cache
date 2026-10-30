#include "c.h"
#include "d.h"
int c_glob = 3;
int d_use(struct c_rec *r) { return r->k + (int)r->l + d_g() + c_glob; }
