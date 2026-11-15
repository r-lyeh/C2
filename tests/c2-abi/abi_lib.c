/* ABI test: the library behind abi_lib.h (built by test.py as a .dll / .so / .dylib) */
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
typedef struct { float x, y, z; } lib_vec3;
EXPORT int lib_add(int a, int b) { return a + b; }
EXPORT lib_vec3 lib_cross(lib_vec3 a, lib_vec3 b) {
    lib_vec3 r = {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    return r;
}
EXPORT double lib_sum(int n, const double *v) { double s = 0; for (int i = 0; i < n; i++) s += v[i]; return s; }
EXPORT const char *lib_name(void) { return "abi_lib"; }
