#ifdef MAIN_CONFIG               // a plain .h sees the includer's macros, and leaks its own, as in C
int plain_cfg = MAIN_CONFIG;
#endif
#define PLAIN_LEAKS 7
