/* ABI test: a C library bound with C2_load (header + shared library), decorated the way library
   headers usually are */
#ifdef _WIN32
#define ABI_LIB_API __declspec(dllimport)
#else
#define ABI_LIB_API __attribute__((visibility("default")))
#endif
typedef struct { float x, y, z; } lib_vec3;
ABI_LIB_API int __cdecl lib_add(int a, int b);
ABI_LIB_API lib_vec3 lib_cross(lib_vec3 a, lib_vec3 b);
ABI_LIB_API double lib_sum(int n, const double *v);
ABI_LIB_API const char *lib_name(void);
