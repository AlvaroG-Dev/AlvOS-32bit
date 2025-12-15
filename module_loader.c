#include "module_loader.h"
#include "memory.h"
#include "string.h"
#include "terminal.h"
#include "kernel.h"
#include "log.h"
#include "mmu.h"

// Variables globales
module_info_t* loaded_modules = NULL;
uint32_t module_count = 0;

// Inicializar el loader de módulos
bool module_loader_init(struct multiboot_tag* mb_info) {
    terminal_printf(&main_terminal, "Initializing module loader...\n");

    if (!mb_info) {
        terminal_printf(&main_terminal, "ERROR: NULL multiboot info pointer\n");
        return false;
    }

    // Verificar magic number y tamaño total
    uint32_t total_size = (uint32_t)mb_info;
    // terminal_printf(&main_terminal, "Multiboot info total size: %u bytes\n", total_size);
    if (total_size < 8) {
        terminal_printf(&main_terminal, "ERROR: Invalid multiboot info size\n");
        return false;
    }

    // Buscar módulos en las tags de Multiboot2
    struct multiboot_tag* tag = (struct multiboot_tag*)((uint8_t*)mb_info + 8);
    module_count = 0;

    // Primera pasada: contar módulos y mostrar todas las tags
    uint32_t offset = 8;
    while (offset < total_size) {
        tag = (struct multiboot_tag*)((uint8_t*)mb_info + offset);

        // Verificar límites
        if (offset + tag->size > total_size) {
            terminal_printf(&main_terminal, "WARNING: Tag exceeds total size, stopping scan\n");
            break;
        }

        // Mostrar información detallada de cada tag
        terminal_printf(&main_terminal, "  Found tag type %u, size %u at offset %u",
                        tag->type, tag->size, offset);

        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_END:
                terminal_printf(&main_terminal, " (END)\n");
                break;

            case MULTIBOOT_TAG_TYPE_CMDLINE:
                terminal_printf(&main_terminal, " (CMDLINE)\n");
                if (tag->size > 8) {
                    struct multiboot_tag_string* cmdline_tag = (struct multiboot_tag_string*)tag;
                    terminal_printf(&main_terminal, "    Kernel cmdline: '%.100s'\n", cmdline_tag->string);
                }
                break;

            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
                terminal_printf(&main_terminal, " (BOOTLOADER_NAME)\n");
                if (tag->size > 8) {
                    struct multiboot_tag_string* name_tag = (struct multiboot_tag_string*)tag;
                    terminal_printf(&main_terminal, "    Bootloader: '%.50s'\n", name_tag->string);
                }
                break;

            case MULTIBOOT_TAG_TYPE_MODULE:
                terminal_printf(&main_terminal, " (MODULE) <-- FOUND MODULE!\n");
                module_count++;
                terminal_printf(&main_terminal, "    -> Module tag found! (count: %u)\n", module_count);
                break;

            case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO:
                terminal_printf(&main_terminal, " (BASIC_MEMINFO)\n");
                break;

            case MULTIBOOT_TAG_TYPE_MMAP:
                terminal_printf(&main_terminal, " (MMAP)\n");
                break;

            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
                terminal_printf(&main_terminal, " (FRAMEBUFFER)\n");
                break;

            default:
                terminal_printf(&main_terminal, " (UNKNOWN TYPE)\n");
                break;
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            terminal_printf(&main_terminal, "  End tag found - stopping scan\n");
            break;
        }

        // Avanzar al siguiente tag (alineado a 8 bytes)
        offset += (tag->size + 7) & ~7;

        // Protección contra bucle infinito
        if (tag->size == 0) {
            terminal_printf(&main_terminal, "WARNING: Zero-sized tag, stopping scan\n");
            break;
        }
    }

    if (module_count == 0) {
        terminal_printf(&main_terminal, "No modules found in multiboot tags\n");
        return true; // No es un error, simplemente no hay módulos
    }

    // Allocar array para información de módulos
    loaded_modules = (module_info_t*)kernel_malloc(sizeof(module_info_t) * module_count);
    if (!loaded_modules) {
        terminal_printf(&main_terminal, "ERROR: Failed to allocate module info array (%u bytes)\n",
                        sizeof(module_info_t) * module_count);
        return false;
    }

    // Segunda pasada: llenar la información de módulos
    offset = 8;
    uint32_t module_index = 0;

    while (offset < total_size && module_index < module_count) {
        tag = (struct multiboot_tag*)((uint8_t*)mb_info + offset);

        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            break;
        }

        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module* module_tag = (struct multiboot_tag_module*)tag;
            terminal_printf(&main_terminal, "Processing module %u:\n", module_index);
            terminal_printf(&main_terminal, "  Tag size: %u\n", tag->size);
            terminal_printf(&main_terminal, "  Module start: 0x%08x\n", module_tag->mod_start);
            terminal_printf(&main_terminal, "  Module end: 0x%08x\n", module_tag->mod_end);

            // Validar direcciones del módulo
            if (module_tag->mod_end <= module_tag->mod_start) {
                terminal_printf(&main_terminal, "ERROR: Invalid module addresses\n");
                offset += (tag->size + 7) & ~7;
                continue;
            }

            loaded_modules[module_index].start = module_tag->mod_start;
            loaded_modules[module_index].end = module_tag->mod_end;
            loaded_modules[module_index].size = module_tag->mod_end - module_tag->mod_start;

            // Mapear la región física del módulo
            if (!mmu_ensure_physical_mapped(loaded_modules[module_index].start,
                                            loaded_modules[module_index].size)) {
                terminal_printf(&main_terminal, "ERROR: Failed to map module phys=0x%08x size=%u\n",
                                loaded_modules[module_index].start, loaded_modules[module_index].size);

                // Cleanup parcial
                for (uint32_t i = 0; i < module_index; i++) {
                    if (loaded_modules[i].cmdline)
                        kernel_free(loaded_modules[i].cmdline);
                }

                kernel_free(loaded_modules);
                loaded_modules = NULL;
                module_count = 0;
                return false;
            }

            // Calcular la dirección virtual mapeada
            uint32_t aligned_start = ALIGN_4KB_DOWN(loaded_modules[module_index].start);
            uint32_t virt_addr = KERNEL_VIRTUAL_BASE + aligned_start;
            loaded_modules[module_index].data = (void*)virt_addr;

            terminal_printf(&main_terminal,
                            "  Mapped module: phys=0x%08x -> virt=0x%08x, size=%u\n",
                            loaded_modules[module_index].start, virt_addr,
                            loaded_modules[module_index].size);
            terminal_printf(&main_terminal, "  Module size: %u bytes\n",
                            loaded_modules[module_index].size);

            // Manejar cmdline
            uint32_t cmdline_offset = sizeof(struct multiboot_tag_module);
            if (tag->size > cmdline_offset) {
                uint32_t cmdline_available = tag->size - cmdline_offset;
                const char* cmdline_start = (const char*)((uint8_t*)module_tag + cmdline_offset);
                uint32_t cmdline_len = 0;

                for (uint32_t i = 0; i < cmdline_available && cmdline_start[i] != '\0'; i++) {
                    cmdline_len++;
                }

                if (cmdline_len > 0) {
                    loaded_modules[module_index].cmdline = (char*)kernel_malloc(cmdline_len + 1);
                    if (loaded_modules[module_index].cmdline) {
                        memcpy(loaded_modules[module_index].cmdline, cmdline_start, cmdline_len);
                        loaded_modules[module_index].cmdline[cmdline_len] = '\0';
                        terminal_printf(&main_terminal, "  Cmdline: '%s' (len: %u)\n",
                                        loaded_modules[module_index].cmdline, cmdline_len);
                    } else {
                        terminal_printf(&main_terminal, "  WARNING: Failed to allocate cmdline\n");
                        loaded_modules[module_index].cmdline = NULL;
                    }
                } else {
                    loaded_modules[module_index].cmdline = NULL;
                    terminal_printf(&main_terminal, "  No cmdline\n");
                }
            } else {
                loaded_modules[module_index].cmdline = NULL;
                terminal_printf(&main_terminal, "  No cmdline (tag too small)\n");
            }

            // Verificar accesibilidad
            uint8_t* module_data = (uint8_t*)loaded_modules[module_index].data;
            terminal_printf(&main_terminal, "  Module data pointer: 0x%08x\n", (uint32_t)module_data);

            if (loaded_modules[module_index].size >= 4) {
                terminal_printf(&main_terminal, "  First 4 bytes: %02x %02x %02x %02x\n",
                                module_data[0], module_data[1], module_data[2], module_data[3]);

                if (module_data[0] == 0x7F && module_data[1] == 'E' &&
                    module_data[2] == 'L' && module_data[3] == 'F') {
                    terminal_printf(&main_terminal, "  -> ELF file detected\n");
                }
            }

            terminal_printf(&main_terminal, "  Module %u loaded successfully\n", module_index);
            module_index++;
        }

        offset += (tag->size + 7) & ~7;
        if (tag->size == 0) break;
    }

    if (module_index != module_count) {
        terminal_printf(&main_terminal, "WARNING: Expected %u modules, but loaded %u\n",
                        module_count, module_index);
    }

    terminal_printf(&main_terminal, "Module loader initialized successfully\n");
    terminal_printf(&main_terminal, "Total modules loaded: %u\n", module_count);

    // Log para debugging
    log_message(LOG_INFO, "Module loader: %u modules loaded", module_count);
    return true;
}


// Buscar módulo por nombre en cmdline
module_info_t* module_find_by_name(const char* name) {
    if (!name || !loaded_modules) {
        return NULL;
    }
    
    terminal_printf(&main_terminal, "Searching for module: '%s'\n", name);
    
    for (uint32_t i = 0; i < module_count; i++) {
        if (!loaded_modules[i].cmdline) {
            continue;
        }
        
        // Buscar el nombre en la cmdline (puede tener path completo)
        char* filename = strrchr(loaded_modules[i].cmdline, '/');
        if (filename) {
            filename++; // Saltar el '/'
        } else {
            filename = loaded_modules[i].cmdline;
        }
        
        terminal_printf(&main_terminal, "  Comparing with module %u: '%s'\n", i, filename);
        
        if (strcmp(filename, name) == 0) {
            terminal_printf(&main_terminal, "  -> Match found!\n");
            return &loaded_modules[i];
        }
    }
    
    terminal_printf(&main_terminal, "  -> No match found\n");
    return NULL;
}

// Obtener módulo por índice
module_info_t* module_get_by_index(uint32_t index) {
    if (index >= module_count || !loaded_modules) {
        return NULL;
    }
    return &loaded_modules[index];
}

// Listar todos los módulos
void module_list_all(void) {
    terminal_printf(&main_terminal, "\n=== Loaded Modules (%u total) ===\n", module_count);
    
    if (module_count == 0) {
        terminal_printf(&main_terminal, "  (none)\n");
        return;
    }
    
    for (uint32_t i = 0; i < module_count; i++) {
        terminal_printf(&main_terminal, 
                       "[%u] 0x%08x-0x%08x (%u bytes)\n",
                       i,
                       loaded_modules[i].start,
                       loaded_modules[i].end,
                       loaded_modules[i].size);
        
        terminal_printf(&main_terminal, "    Cmdline: %s\n",
                       loaded_modules[i].cmdline ? 
                       loaded_modules[i].cmdline : "(none)");
        
        terminal_printf(&main_terminal, "    Data ptr: 0x%08x\n", 
                       (uint32_t)loaded_modules[i].data);
        
        // Mostrar primeros bytes del módulo si es accesible
        if (loaded_modules[i].size >= 8 && loaded_modules[i].data) {
            uint8_t* data = (uint8_t*)loaded_modules[i].data;
            terminal_printf(&main_terminal, 
                           "    Header: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                           data[0], data[1], data[2], data[3],
                           data[4], data[5], data[6], data[7]);
            
            // Detectar tipo de archivo
            if (data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
                terminal_printf(&main_terminal, "    Type: ELF executable\n");
            } else if (data[0] == 'M' && data[1] == 'Z') {
                terminal_printf(&main_terminal, "    Type: DOS/PE executable\n");
            } else {
                terminal_printf(&main_terminal, "    Type: Unknown/Data\n");
            }
        }
        terminal_printf(&main_terminal, "\n");
    }
}

// Función de debugging para verificar la estructura multiboot
void module_debug_multiboot_info(struct multiboot_tag* mb_info) {
    if (!mb_info) {
        terminal_printf(&main_terminal, "ERROR: NULL multiboot info\n");
        return;
    }
    
    terminal_printf(&main_terminal, "\n=== Multiboot2 Debug Info ===\n");
    
    uint32_t total_size = *(uint32_t*)mb_info;
    terminal_printf(&main_terminal, "Total size: %u bytes\n", total_size);
    terminal_printf(&main_terminal, "Multiboot info address: 0x%08x\n", (uint32_t)mb_info);
    
    uint32_t offset = 8;
    uint32_t tag_count = 0;
    
    while (offset < total_size) {
        struct multiboot_tag* tag = (struct multiboot_tag*)((uint8_t*)mb_info + offset);
        
        if (offset + sizeof(struct multiboot_tag) > total_size) {
            terminal_printf(&main_terminal, "WARNING: Tag header exceeds bounds\n");
            break;
        }
        
        terminal_printf(&main_terminal, "\nTag %u at offset %u:\n", tag_count, offset);
        terminal_printf(&main_terminal, "  Type: %u", tag->type);
        
        // Mostrar nombre del tipo si es conocido
        switch (tag->type) {
            case MULTIBOOT_TAG_TYPE_END: 
                terminal_printf(&main_terminal, " (END)"); break;
            case MULTIBOOT_TAG_TYPE_CMDLINE: 
                terminal_printf(&main_terminal, " (CMDLINE)"); break;
            case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME: 
                terminal_printf(&main_terminal, " (BOOTLOADER_NAME)"); break;
            case MULTIBOOT_TAG_TYPE_MODULE: 
                terminal_printf(&main_terminal, " (MODULE)"); break;
            case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO: 
                terminal_printf(&main_terminal, " (BASIC_MEMINFO)"); break;
            case MULTIBOOT_TAG_TYPE_MMAP: 
                terminal_printf(&main_terminal, " (MMAP)"); break;
            case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: 
                terminal_printf(&main_terminal, " (FRAMEBUFFER)"); break;
            default: 
                terminal_printf(&main_terminal, " (UNKNOWN)"); break;
        }
        terminal_printf(&main_terminal, "\n");
        
        terminal_printf(&main_terminal, "  Size: %u bytes\n", tag->size);
        
        if (tag->type == MULTIBOOT_TAG_TYPE_END) {
            terminal_printf(&main_terminal, "  -> End of tags\n");
            break;
        }
        
        if (tag->type == MULTIBOOT_TAG_TYPE_MODULE) {
            struct multiboot_tag_module* mod = (struct multiboot_tag_module*)tag;
            terminal_printf(&main_terminal, "  Module start: 0x%08x\n", mod->mod_start);
            terminal_printf(&main_terminal, "  Module end: 0x%08x\n", mod->mod_end);
            terminal_printf(&main_terminal, "  Module size: %u bytes\n", 
                           mod->mod_end - mod->mod_start);
            
            if (tag->size > sizeof(struct multiboot_tag_module)) {
                terminal_printf(&main_terminal, "  Cmdline: '%.50s'\n", mod->cmdline);
            }
        }
        
        offset += (tag->size + 7) & ~7;
        tag_count++;
        
        if (tag->size == 0) {
            terminal_printf(&main_terminal, "WARNING: Zero-sized tag, stopping\n");
            break;
        }
        
        if (tag_count > 50) {
            terminal_printf(&main_terminal, "WARNING: Too many tags, stopping\n");
            break;
        }
    }
    
    terminal_printf(&main_terminal, "\nTotal tags processed: %u\n", tag_count);
}

// Función específica para verificar la configuración de GRUB
void module_check_grub_config(void) {
    terminal_printf(&main_terminal, "\n=== GRUB Configuration Diagnostic ===\n");
    terminal_printf(&main_terminal, "Expected GRUB configuration should look like:\n");
    terminal_printf(&main_terminal, "menuentry \"Your OS\" {\n");
    terminal_printf(&main_terminal, "    multiboot2 /boot/kernel.bin\n");
    terminal_printf(&main_terminal, "    module2 /boot/module1.bin module1_name\n");
    terminal_printf(&main_terminal, "    module2 /boot/module2.bin module2_name\n");
    terminal_printf(&main_terminal, "    boot\n");
    terminal_printf(&main_terminal, "}\n\n");
    
    terminal_printf(&main_terminal, "Common issues:\n");
    terminal_printf(&main_terminal, "1. Using 'module' instead of 'module2'\n");
    terminal_printf(&main_terminal, "2. Missing files in /boot/ directory\n");
    terminal_printf(&main_terminal, "3. Incorrect file paths\n");
    terminal_printf(&main_terminal, "4. GRUB not finding the modules on disk\n\n");
}

// Verificar si el magic number de multiboot2 es correcto
bool module_verify_multiboot_magic(uint32_t magic) {
    terminal_printf(&main_terminal, "Checking multiboot2 magic...\n");
    terminal_printf(&main_terminal, "Received magic: 0x%08x\n", magic);
    terminal_printf(&main_terminal, "Expected magic: 0x36d76289\n");
    
    if (magic == 0x36d76289) {
        terminal_printf(&main_terminal, "✓ Multiboot2 magic is CORRECT\n");
        return true;
    } else {
        terminal_printf(&main_terminal, "✗ Multiboot2 magic is WRONG!\n");
        if (magic == 0x2BADB002) {
            terminal_printf(&main_terminal, "  This is Multiboot1 magic - check your GRUB config\n");
        }
        return false;
    }
}

// Cleanup del module loader
void module_loader_cleanup(void) {
    if (loaded_modules) {
        // Liberar cmdlines
        for (uint32_t i = 0; i < module_count; i++) {
            if (loaded_modules[i].cmdline) {
                kernel_free(loaded_modules[i].cmdline);
            }
        }
        
        // Liberar array principal
        kernel_free(loaded_modules);
        loaded_modules = NULL;
    }
    
    module_count = 0;
}