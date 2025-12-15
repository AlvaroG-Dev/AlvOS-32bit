#include "math_utils.h"

// Implementaciones aproximadas para entornos bare-metal
float sqrt(float x) {
    if (x <= 0) return 0;
    float result = x;
    for (int i = 0; i < 10; i++) {
        result = 0.5f * (result + x / result);
    }
    return result;
}

float cos(float x) {
    // Aproximación de Taylor para cos(x)
    while (x > 3.14159265f * 2) x -= 3.14159265f * 2;
    while (x < 0) x += 3.14159265f * 2;
    
    float x2 = x * x;
    float x4 = x2 * x2;
    return 1.0f - x2/2.0f + x4/24.0f - (x2*x4)/720.0f;
}

float sin(float x) {
    // Aproximación de Taylor para sin(x)
    while (x > 3.14159265f * 2) x -= 3.14159265f * 2;
    while (x < 0) x += 3.14159265f * 2;
    
    float x3 = x * x * x;
    float x5 = x3 * x * x;
    return x - x3/6.0f + x5/120.0f;
}

