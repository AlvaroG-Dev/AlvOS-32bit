#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>

// Descargar un archivo desde una URL y guardarlo en el sistema de archivos
// Soporta URLs simples tipo: http://ejemplo.com/archivo.txt
bool http_download(const char *url, const char *dest_path);

#endif // HTTP_H
