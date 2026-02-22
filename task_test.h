#ifndef TASK_TEST_H
#define TASK_TEST_H

#ifdef __cplusplus
extern "C" {
#endif

void run_task_utils_test_suite(void);
void test_task_utils_command(void);
void run_integration_tests(void);      // ✅ NUEVO: Tests de integración del sistema

#ifdef __cplusplus
}
#endif

#endif // TASK_TEST_H
