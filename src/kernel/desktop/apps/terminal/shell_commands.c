#include "shell_commands.h"
#include "terminal.h"
#include "../../graphics/graphics.h"

static uint8_t shell_command_matches(const char *line, uint32_t length,
                                     const char *command,
                                     uint32_t command_length) {
  if (!line || length != command_length)
    return 0;
  for (uint32_t i = 0; i < command_length; i++) {
    if (line[i] != command[i])
      return 0;
  }
  return 1;
}

static const char *shell_font_family_name(gfx_font_family_t family) {
  switch (family) {
    case GFX_FONT_LATO: return "Lato";
    case GFX_FONT_DEJAVU_SANS: return "DejaVu Sans";
    case GFX_FONT_DEJAVU_MONO: return "DejaVu Sans Mono";
    case GFX_FONT_NOTO_SANS: return "Noto Sans";
    case GFX_FONT_NOTO_SERIF: return "Noto Serif";
    case GFX_FONT_INTER:
    default: return "Inter";
  }
}

static uint32_t shell_command_u32_to_dec(char out[12], uint32_t value) {
  char reversed[12];
  uint32_t count = 0;

  if (value == 0) {
    out[0] = '0';
    out[1] = 0;
    return 1;
  }

  while (value && count < 11u) {
    reversed[count++] = (char)('0' + (value % 10u));
    value /= 10u;
  }

  for (uint32_t i = 0; i < count; i++)
    out[i] = reversed[count - 1u - i];

  out[count] = 0;
  return count;
}

uint8_t shell_commands_execute(const char *line, uint32_t length,
                              uint32_t tick, int32_t pointer_x,
                              int32_t pointer_y, uint32_t *random_state) {
  char buffer[12];

  if (!line)
    return 0;

  if (shell_command_matches(line, length, "help", 4u)) {
#ifdef VEXOS_CI_INPUT_TEST
    extern void serial_print(const char *str);
    serial_print("[CI] Shell help command executed\n");
#endif
    terminal_append("help clear cls echo mem ver uptime pos rand specs ring3 fs explorer view notepad setup wall font\n");
    return 1;
  }

  if (length >= 4u && line[0] == 'f' && line[1] == 'o' &&
      line[2] == 'n' && line[3] == 't' &&
      (length == 4u || line[4] == ' ')) {
    uint32_t pos = 4u;
    while (pos < length && line[pos] == ' ') pos++;

    if (pos == length) {
      terminal_append("Current: ");
      terminal_append(shell_font_family_name(gfx_get_font_family()));
      terminal_append(" ");
      shell_command_u32_to_dec(buffer, gfx_get_font_size());
      terminal_append(buffer);
      terminal_append("px ");
      terminal_append(gfx_get_font_bold() ? "bold\\n" : "regular\\n");
      terminal_append("Families: inter, lato, dejavu, mono, noto-sans, noto-serif\\n");
      terminal_append("Usage: font <family> [6-96] [bold|regular] | font reset\\n");
      return 1;
    }

    uint32_t token_start = pos;
    while (pos < length && line[pos] != ' ') pos++;
    uint32_t token_length = pos - token_start;

    if (shell_command_matches(line + token_start, token_length, "reset", 5u)) {
      while (pos < length && line[pos] == ' ') pos++;
      if (pos != length) {
        terminal_append("Usage: font reset\\n");
        return 1;
      }
      gfx_set_font(GFX_FONT_INTER, 1u, 0u);
      gfx_set_font_size(16u);
      terminal_append("Font reset to Inter 16px regular.\\n");
      return 1;
    }

    gfx_font_family_t family = gfx_get_font_family();
    uint8_t scale = gfx_get_font_scale();
    uint8_t bold = gfx_get_font_bold();
    uint16_t pixel_height = gfx_get_font_size();

    if (shell_command_matches(line + token_start, token_length, "inter", 5u))
      family = GFX_FONT_INTER;
    else if (shell_command_matches(line + token_start, token_length, "lato", 4u))
      family = GFX_FONT_LATO;
    else if (shell_command_matches(line + token_start, token_length, "dejavu", 6u))
      family = GFX_FONT_DEJAVU_SANS;
    else if (shell_command_matches(line + token_start, token_length, "mono", 4u))
      family = GFX_FONT_DEJAVU_MONO;
    else if (shell_command_matches(line + token_start, token_length, "noto-sans", 9u) ||
             shell_command_matches(line + token_start, token_length, "sans", 4u))
      family = GFX_FONT_NOTO_SANS;
    else if (shell_command_matches(line + token_start, token_length, "noto-serif", 10u) ||
             shell_command_matches(line + token_start, token_length, "serif", 5u))
      family = GFX_FONT_NOTO_SERIF;
    else {
      terminal_append("Unknown font. Use inter, lato, dejavu, mono, noto-sans, or noto-serif.\\n");
      return 1;
    }

    while (pos < length) {
      while (pos < length && line[pos] == ' ') pos++;
      if (pos >= length) break;

      token_start = pos;
      while (pos < length && line[pos] != ' ') pos++;
      token_length = pos - token_start;

      if (shell_command_matches(line + token_start, token_length, "bold", 4u)) {
        bold = 1u;
      } else if (shell_command_matches(line + token_start, token_length, "regular", 7u)) {
        bold = 0u;
      } else {
        uint32_t parsed_size = 0;
        uint8_t numeric = token_length != 0u;
        for (uint32_t i = 0; i < token_length; ++i) {
          char digit = line[token_start + i];
          if (digit < '0' || digit > '9') {
            numeric = 0u;
            break;
          }
          parsed_size = parsed_size * 10u + (uint32_t)(digit - '0');
          if (parsed_size > 96u) {
            numeric = 0u;
            break;
          }
        }
        if (!numeric || parsed_size < 6u || parsed_size > 96u) {
          terminal_append("Use a font size from 6 to 96, plus optional bold or regular.\\n");
          return 1;
        }
        pixel_height = (uint16_t)parsed_size;
      }
    }

    gfx_set_font(family, scale, bold);
    gfx_set_font_size(pixel_height);
    terminal_append("Font updated: ");
    terminal_append(shell_font_family_name(family));
    terminal_append(" ");
    shell_command_u32_to_dec(buffer, pixel_height);
    terminal_append(buffer);
    terminal_append("px ");
    terminal_append(bold ? "bold.\\n" : "regular.\\n");
    return 1;
  }

  if (shell_command_matches(line, length, "clear", 5u) ||
      shell_command_matches(line, length, "cls", 3u)) {
    terminal_clear_output();
    return 1;
  }

  if (length >= 4u && line[0] == 'e' && line[1] == 'c' &&
      line[2] == 'h' && line[3] == 'o') {
    uint32_t i = 4u;
    while (i < length && line[i] == ' ')
      i++;
    for (; i < length; i++) {
      char character[2] = {line[i], 0};
      terminal_append(character);
    }
    terminal_append("\n");
    return 1;
  }

  if (shell_command_matches(line, length, "mem", 3u)) {
#ifdef VEXOS_CI_INPUT_TEST
    extern void serial_print(const char *str);
    serial_print("[CI] Shell mem command executed\n");
#endif
    terminal_append("heap ok\n");
    return 1;
  }

  if (shell_command_matches(line, length, "ver", 3u)) {
    terminal_append("tiny64 ui\n");
    return 1;
  }

  if (shell_command_matches(line, length, "uptime", 6u)) {
    shell_command_u32_to_dec(buffer, tick);
    terminal_append(buffer);
    terminal_append(" ticks\n");
    return 1;
  }

  if (shell_command_matches(line, length, "pos", 3u)) {
    terminal_append("x=");
    shell_command_u32_to_dec(buffer, (uint32_t)pointer_x);
    terminal_append(buffer);
    terminal_append(" y=");
    shell_command_u32_to_dec(buffer, (uint32_t)pointer_y);
    terminal_append(buffer);
    terminal_append("\n");
    return 1;
  }

  if (shell_command_matches(line, length, "rand", 4u)) {
    if (!random_state)
      return 0;
    *random_state = (*random_state * 1664525u) + 1013904223u;
    shell_command_u32_to_dec(buffer, *random_state);
    terminal_append(buffer);
    terminal_append("\n");
    return 1;
  }

  return 0;
}
