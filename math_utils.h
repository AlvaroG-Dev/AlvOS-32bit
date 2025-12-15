// math_utils.h
#ifndef MATH_UTILS_H
#define MATH_UTILS_H

static inline int min(int a, int b) {
    return a < b ? a : b;
}

static inline int max(int a, int b) {
    return a > b ? a : b;
}

static inline int abs(int x) {
    return (x < 0) ? -x : x;
}


// Añade estas declaraciones
float sqrt(float x);
float cos(float x);
float sin(float x);

#endif