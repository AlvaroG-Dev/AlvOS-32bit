#!/usr/bin/env python3
import struct

def create_keyboard_layout(filename, name, normal_map, shift_map, altgr_map=None):
    """
    Crea un archivo binario de layout de teclado compatible con tu SO.
    normal_map, shift_map y altgr_map deben ser listas de caracteres o enteros.
    """
    def to_byte_list(lst):
        # Mapa explícito para caracteres especiales en Latin-1
        special_chars = {
            '€': 0x80,  # Euro (Latin-1: 128)
            'ñ': 0xF1,  # ñ (Latin-1: 241)
            'ç': 0xE7,  # ç (Latin-1: 231)
            'Ñ': 0xD1,  # Ñ (Latin-1: 209)
            'Ç': 0xC7,  # Ç (Latin-1: 199)
            '·': 0xB7,  # Middle dot (Latin-1: 183)
            '¿': 0xBF,  # Inverted question mark (Latin-1: 191)
            '¡': 0xA1,  # Inverted exclamation mark (Latin-1: 161)
            '´': 0xB4,  # Acute accent (Latin-1: 180)
            '¨': 0xA8   # Diaeresis (Latin-1: 168)
        }

        bytes_list = []
        for x in lst:
            if isinstance(x, str):
                if len(x) != 1:
                    raise ValueError(f"Cada elemento debe ser un caracter, no '{x}'")
                # Usar mapeo especial si está definido, sino ord() para ASCII
                byte_val = special_chars.get(x, ord(x))
                if byte_val > 255:
                    raise ValueError(f"Caracter '{x}' excede rango de bytes (0-255)")
                bytes_list.append(byte_val)
            elif isinstance(x, int):
                if x < 0 or x > 255:
                    raise ValueError(f"Valor entero {x} fuera de rango (0-255)")
                bytes_list.append(x)
            else:
                raise ValueError(f"Elemento inválido en el mapa: {x}")
        # Rellenar hasta 128 con ceros
        return (bytes_list + [0]*128)[:128]

    # Convertir mapas a bytes
    normal_bytes = bytes(to_byte_list(normal_map))
    shift_bytes = bytes(to_byte_list(shift_map))
    altgr_bytes = bytes(to_byte_list(altgr_map)) if altgr_map else bytes([0]*128)

    # Magic number 'KBLY' = 0x4B424C59
    magic = 0x4B424C59

    # Nombre del layout (32 bytes, null-terminated, ASCII)
    name_bytes = name.encode('ascii')[:31] + b'\0'
    name_bytes += b'\0' * (32 - len(name_bytes))

    # Empaquetar datos: magic (4 bytes) + name (32 bytes) + normal (128) + shift (128) + altgr (128)
    data = struct.pack('<I', magic) + name_bytes + normal_bytes + shift_bytes + altgr_bytes

    with open(filename, 'wb') as f:
        f.write(data)

    print(f"Layout '{name}' guardado en {filename} ({len(data)} bytes)")

# Layout español corregido (PS/2 set 2, teclado español de España)
normal = [
    '\x1B', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', ' ', '¡', '\b',  # 0x00-0x0E
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '`', '+', '\n',           # 0x0F-0x1C
    '\0', 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'ñ', '´', 'ç',                 # 0x1D-0x29
    '\0', '<', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', '\0',                 # 0x2A-0x36
    '*', '\0', ' ', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '7',  # 0x37-0x46
    '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', '\0', '\0', '\0', '\0'  # 0x47-0x56
]

shift = [
    '\x1B', '!', '"', '·', '$', '%', '&', '/', '(', ')', '=', '?', '¿', '\b',  # 0x00-0x0E
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '^', '*', '\n',           # 0x0F-0x1C
    '\0', 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 'Ñ', '¨', 'Ç',                 # 0x1D-0x29
    '\0', '>', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', '\0',                 # 0x2A-0x36
    '*', '\0', ' ', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '7',  # 0x37-0x46
    '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', '\0', '\0', '\0', '\0'  # 0x47-0x56
]

altgr = [
    '\0', '\x1B', '|', '@', '#', '~', '\0', '\0', '{', '[', ']', '}', '\\', '|', '\b',  # 0x00-0x0E
    '\t', '\0', '\0', '€', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '[', ']', '\n',   # 0x0F-0x1C
    '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '{', '}',       # 0x1D-0x29
    '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0',      # 0x2A-0x36
    '*', '\0', ' ', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '\0', '7',  # 0x37-0x46
    '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.', '\0', '\0', '\0', '\0'  # 0x47-0x56
]

if __name__ == "__main__":
    create_keyboard_layout("es-kbd.kbd", "ES-QWERTY", normal, shift, altgr)