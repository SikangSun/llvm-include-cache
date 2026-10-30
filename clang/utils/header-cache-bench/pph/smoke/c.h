#ifndef C_H
#define C_H
struct c_rec { int k; long l; };
typedef int c_int;
enum c_e { C_A, C_B = 5 };
extern int c_glob;
static inline int c_f(int x) { return x + C_B; }
#endif
