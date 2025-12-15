// test_program.c - Programa de prueba simple
void _start(void) {
    // Punto de entrada del programa
    // En un sistema real tendrías acceso a syscalls, pero por ahora hacemos algo básico
    
    // Variable local para contar
    volatile int counter = 0;
    
    // Comportamiento del programa
    while (counter < 5) {
        counter++;
        
        // Pequeña pausa (en un sistema real usarías task_sleep)
        for (volatile int i = 0; i < 1000000; i++);
    }
    
    // El programa termina cuando esta función retorna
    // En modo kernel, esto causará que la tarea termine
}