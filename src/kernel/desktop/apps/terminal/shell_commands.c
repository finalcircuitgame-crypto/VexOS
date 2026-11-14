#include "shell_commands.h"
#include "terminal.h"

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
