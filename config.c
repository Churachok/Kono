#include "server.h"
#include <wlr/util/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#define MAX_BINDS 128
#define MAX_PATH 256
#define MAX_LINE 512

struct keybind {
    uint32_t mod;
    xkb_keysym_t keysym;
    char command[MAX_PATH];
};

static struct keybind binds[MAX_BINDS];
static int num_binds = 0;

// Парсинг модификаторов для kono.conf
static uint32_t parse_mod(const char *mod) {
    if (strcmp(mod, "Super") == 0 || strcmp(mod, "SUPER") == 0) 
        return WLR_MODIFIER_LOGO;
    if (strcmp(mod, "Alt") == 0 || strcmp(mod, "ALT") == 0) 
        return WLR_MODIFIER_ALT;
    if (strcmp(mod, "Ctrl") == 0 || strcmp(mod, "CTRL") == 0) 
        return WLR_MODIFIER_CTRL;
    if (strcmp(mod, "Shift") == 0 || strcmp(mod, "SHIFT") == 0) 
        return WLR_MODIFIER_SHIFT;
    return 0;
}

// Парсинг клавиш для kono.conf
static xkb_keysym_t parse_key(const char *key) {
    // Буквы
    if (strlen(key) == 1 && key[0] >= 'a' && key[0] <= 'z')
        return XKB_KEY_a + (key[0] - 'a');
    if (strlen(key) == 1 && key[0] >= 'A' && key[0] <= 'Z')
        return XKB_KEY_A + (key[0] - 'A');
    
    // Цифры
    if (strlen(key) == 1 && key[0] >= '0' && key[0] <= '9')
        return XKB_KEY_0 + (key[0] - '0');
    
    // Специальные символы
    if (strcmp(key, "Return") == 0 || strcmp(key, "Enter") == 0) 
        return XKB_KEY_Return;
    if (strcmp(key, "Space") == 0) return XKB_KEY_space;
    if (strcmp(key, "Tab") == 0) return XKB_KEY_Tab;
    if (strcmp(key, "Escape") == 0 || strcmp(key, "Esc") == 0) 
        return XKB_KEY_Escape;
    if (strcmp(key, "BackSpace") == 0) return XKB_KEY_BackSpace;
    if (strcmp(key, "Delete") == 0) return XKB_KEY_Delete;
    
    // Стрелки
    if (strcmp(key, "Up") == 0) return XKB_KEY_Up;
    if (strcmp(key, "Down") == 0) return XKB_KEY_Down;
    if (strcmp(key, "Left") == 0) return XKB_KEY_Left;
    if (strcmp(key, "Right") == 0) return XKB_KEY_Right;
    
    // Навигация
    if (strcmp(key, "Home") == 0) return XKB_KEY_Home;
    if (strcmp(key, "End") == 0) return XKB_KEY_End;
    if (strcmp(key, "Page_Up") == 0) return XKB_KEY_Page_Up;
    if (strcmp(key, "Page_Down") == 0) return XKB_KEY_Page_Down;
    
    // F-клавиши
    if (strcmp(key, "F1") == 0) return XKB_KEY_F1;
    if (strcmp(key, "F2") == 0) return XKB_KEY_F2;
    if (strcmp(key, "F3") == 0) return XKB_KEY_F3;
    if (strcmp(key, "F4") == 0) return XKB_KEY_F4;
    if (strcmp(key, "F5") == 0) return XKB_KEY_F5;
    if (strcmp(key, "F6") == 0) return XKB_KEY_F6;
    if (strcmp(key, "F7") == 0) return XKB_KEY_F7;
    if (strcmp(key, "F8") == 0) return XKB_KEY_F8;
    if (strcmp(key, "F9") == 0) return XKB_KEY_F9;
    if (strcmp(key, "F10") == 0) return XKB_KEY_F10;
    if (strcmp(key, "F11") == 0) return XKB_KEY_F11;
    if (strcmp(key, "F12") == 0) return XKB_KEY_F12;
    
    // Print, Scroll, Pause
    if (strcmp(key, "Print") == 0) return XKB_KEY_Print;
    if (strcmp(key, "Scroll_Lock") == 0) return XKB_KEY_Scroll_Lock;
    if (strcmp(key, "Pause") == 0) return XKB_KEY_Pause;
    
    // Дополнительные клавиши
    if (strcmp(key, "Insert") == 0) return XKB_KEY_Insert;
    if (strcmp(key, "Menu") == 0) return XKB_KEY_Menu;
    if (strcmp(key, "Caps_Lock") == 0) return XKB_KEY_Caps_Lock;
    if (strcmp(key, "Num_Lock") == 0) return XKB_KEY_Num_Lock;
    
    wlr_log(WLR_ERROR, "Unknown key: %s", key);
    return XKB_KEY_NoSymbol;
}

// Обрезка пробелов в начале и конце строки
static char *trim(char *str) {
    while (*str == ' ' || *str == '\t') str++;
    char *end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n')) {
        *end-- = '\0';
    }
    return str;
}

// Парсинг строки с биндом в формате kono.conf
static void parse_bind_line(char *line) {
    // Формат: bind = Mod+Key, command
    char *equal = strchr(line, '=');
    if (!equal) return;
    
    // Левая часть
    *equal = '\0';
    char *left = trim(line);
    char *right = trim(equal + 1);
    
    if (strlen(left) == 0 || strlen(right) == 0) return;
    
    // Парсим модификаторы и клавишу из левой части
    char keycombo[256] = {0};
    strncpy(keycombo, left, sizeof(keycombo) - 1);
    
    // Ищем команду в правой части
    char *comma = strchr(right, ',');
    char *command = right;
    if (comma) {
        *comma = '\0';
        command = trim(comma + 1);
    }
    
    // Парсим комбинацию клавиш (например, "Super+Shift+D")
    uint32_t mod = 0;
    char *last_plus = strrchr(keycombo, '+');
    char *keystr;
    
    if (last_plus) {
        keystr = last_plus + 1;
        *last_plus = '\0';
        
        // Парсим модификаторы
        char *token = strtok(keycombo, "+");
        while (token) {
            mod |= parse_mod(trim(token));
            token = strtok(NULL, "+");
        }
    } else {
        keystr = keycombo;
    }
    
    xkb_keysym_t sym = parse_key(trim(keystr));
    if (sym == XKB_KEY_NoSymbol) {
        wlr_log(WLR_ERROR, "Invalid key in bind: %s", keystr);
        return;
    }
    
    // Добавляем бинд
    if (num_binds < MAX_BINDS) {
        binds[num_binds].mod = mod;
        binds[num_binds].keysym = sym;
        strncpy(binds[num_binds].command, command, MAX_PATH - 1);
        binds[num_binds].command[MAX_PATH - 1] = '\0';
        
        wlr_log(WLR_INFO, "Loaded bind: %s -> %s (mod=0x%x, keysym=%u)", 
                left, command, mod, sym);
        num_binds++;
    }
}

bool load_kono_config(const char *config_path) {
    FILE *f = fopen(config_path, "r");
    if (!f) {
        wlr_log(WLR_INFO, "No config file at %s, using defaults", config_path);
        return false;
    }
    
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *trimmed = trim(line);
        
        // Пропускаем комментарии и пустые строки
        if (trimmed[0] == '#' || trimmed[0] == '\0') continue;
        
        // Ищем строки с bind
        if (strncmp(trimmed, "bind", 4) == 0) {
            parse_bind_line(trimmed + 4);
        }
    }
    
    fclose(f);
    wlr_log(WLR_INFO, "Loaded %d keybinds from %s", num_binds, config_path);
    return true;
}

bool handle_keybinding(xkb_keysym_t sym, uint32_t mods) {
    for (int i = 0; i < num_binds; i++) {
        if (binds[i].keysym == sym && binds[i].mod == mods) {
            wlr_log(WLR_INFO, "Executing: %s", binds[i].command);
            
            pid_t pid = fork();
            if (pid == 0) {
                setsid();
                
                // Передаем окружение Wayland
                const char *display = getenv("WAYLAND_DISPLAY");
                const char *runtime = getenv("XDG_RUNTIME_DIR");
                if (display) setenv("WAYLAND_DISPLAY", display, 1);
                if (runtime) setenv("XDG_RUNTIME_DIR", runtime, 1);
                
                // Выполняем через shell
                execl("/bin/sh", "/bin/sh", "-c", binds[i].command, NULL);
                exit(1);
            } else if (pid > 0) {
                // Не ждем завершения, чтобы не блокировать цикл
                waitpid(pid, NULL, WNOHANG);
            }
            
            return true;
        }
    }
    return false;
}