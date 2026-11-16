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
    terminal_append("help clear cls echo mem ver uptime pos rand specs ring3 fs explorer view notepad setup wall\n");
    return 1;
  }

  if (length >= 4u && line[0] == 'f' && line[1] == 'o' &&
      line[2] == 'n' && line[3] == 't' &&
      (length == 4u || line[4] == ' ')) {
    uint32_t pos = 4u;
    while (pos < length && line[pos] == ' ') pos++;
    uint32_t token_start = pos;
    while (pos < length && line[pos] != ' ') pos++;
    uint32_t token_length = pos - token_start;
    gfx_font_family_t family = gfx_get_font_family();
    uint8_t scale = gfx_get_font_scale();
    uint8_t bold = gfx_get_font_bold();

    if (!token_length) {
      terminal_append("Fonts: inter, lato, dejavu, mono\\n");
      terminal_append("Usage: font <family> [1|2|3] [bold|regular]\\n");
      return 1;
    }
    if (token_length == 5u &&
        line[token_start] == 'i' && line[token_start + 1u] == 'n' &&
        line[token_start + 2u] == 't' && line[token_start + 3u] == 'e' &&
        line[token_start + 4u] == 'r') family = GFX_FONT_INTER;
    else if (token_length == 4u &&
        line[token_start] == 'l' && line[token_start + 1u] == 'a' &&
        line[token_start + 2u] == 't' && line[token_start + 3u] == 'o')
      family = GFX_FONT_LATO;
    else if (token_length == 6u &&
        line[token_start] == 'd' && line[token_start + 1u] == 'e' &&
        line[token_start + 2u] == 'j' && line[token_start + 3u] == 'a' &&
        line[token_start + 4u] == 'v' && line[token_start + 5u] == 'u')
      family = GFX_FONT_DEJAVU_SANS;
    else if (token_length == 4u &&
        line[token_start] == 'm' && line[token_start + 1u] == 'o' &&
        line[token_start + 2u] == 'n' && line[token_start + 3u] == 'o')
      family = GFX_FONT_DEJAVU_MONO;
    else {
      terminal_append("Unknown font. Use: inter, lato, dejavu, mono\\n");
      return 1;
    }

    while (pos < length && line[pos] == ' ') pos++;
    if (pos < length) {
      if (line[pos] >= '1' && line[pos] <= '3' &&
          (pos + 1u == length || line[pos + 1u] == ' ')) {
        scale = (uint8_t)(line[pos] - '0');
        pos++;
        while (pos < length && line[pos] == ' ') pos++;
      }
      uint32_t style_length = length - pos;
      if (style_length == 4u && line[pos] == 'b' &&
          line[pos + 1u] == 'o' && line[pos + 2u] == 'l' &&
          line[pos + 3u] == 'd') bold = 1u;
      else if (style_length == 7u && line[pos] == 'r' &&
          line[pos + 1u] == 'e' && line[pos + 2u] == 'g' &&
          line[pos + 3u] == 'u' && line[pos + 4u] == 'l' &&
          line[pos + 5u] == 'a' && line[pos + 6u] == 'r') bold = 0u;
      else if (style_length) {
        terminal_append("Style must be bold or regular.\\n");
        return 1;
      }
    }
    gfx_set_font(family, scale, bold);
    terminal_append("System font updated.\\n");
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
