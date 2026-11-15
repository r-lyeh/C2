//#include <stdio.h>
#define NULL ((void*)0)

#define LIMIT 42

typedef int MyInt;

int global_counter = 10;

struct Point {
    int x;
    int y;
};

void* get_null_ptr() {
    return NULL;
}

int add(int a, int b) {
    return (a, a + b); // comma operator
}

int main() {
    MyInt acc = 0;
    char ch = 65;
    double pi = 3.14;
    void *empty = get_null_ptr();

    // Arrays and indexing
    int arr[3];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = LIMIT;

    // for loop with continue
    for (int i = 0; i < 3; i = i + 1) {
        if (arr[i] == 20) {
            continue;
        }
        acc = acc + arr[i]; // 10 + 42 = 52
    }

    // while loop with break
    int w = 0;
    while (w < 5) {
        w = w + 1;
        if (w == 3) {
            break;
        }
    }

    // do-while loop
    int d = 0;
    do {
        d = d + 1;
    } while (d < 2);

    // Pointers & dereferencing
    int val = 5;
    int *vptr = &val;
    *vptr = *vptr + 1; // val becomes 6

    // Structs and pointers to structs (->)
    struct Point pt;
    pt.x = global_counter; // 10
    pt.y = *vptr;          // 6

    struct Point *p = &pt;
    p->x = p->x + 2;       // pt.x becomes 12

    // switch / case / default / break
    switch (d) {
        case 1:
            acc = 0;
            break;
        case 2:
            acc = add(acc, p->x); // 52 + 12 = 64
            break;
        default:
            acc = 999;
            break;
    }

    return acc; // Should evaluate to 64
}
