/* Settings file (see settings.h) */
#include <stdio.h>
#include <string.h>
#include "settings.h"

int settings_load(const char *path, const setting *settings, int count)
{
  FILE *file = fopen(path, "r");
  char line[256];

  if (!file) return 0;
  while (fgets(line, sizeof(line), file))
  {
    char name[32];
    int value, i;
    if (sscanf(line, " %31[a-z_0-9] = %d", name, &value) != 2) continue;
    if (value < 0 || value > 31) continue;
    for (i = 0; i < count; i++)
      if (!strcmp(name, settings[i].name) && (settings[i].allowed >> value & 1)) *settings[i].value = value;
  }
  fclose(file);
  return 1;
}

int settings_save(const char *path, const char *header, const setting *settings, int count)
{
  FILE *file = fopen(path, "w");
  int i;

  if (!file) return 0;
  fprintf(file, "; %s\n", header);
  for (i = 0; i < count; i++)
    fprintf(file, "%s = %d    ; %s\n", settings[i].name, *settings[i].value, settings[i].comment);
  return fclose(file) == 0;
}

#ifdef SETTINGS_TEST
/* make test: values round-trip; comments, unknown names and values not allowed are ignored */
#include <assert.h>
int main(void)
{
  const char *path = "build/settings_test.ini";
  int scale = 4, flag = 1;
  setting table[] = {
    { "scale", &scale, 1 << 0 | 1 << 1 | 1 << 2 | 1 << 4, "1, 2, 4 or 0" },
    { "flag", &flag, 3, "on or off" },
  };
  FILE *file;

  assert(settings_save(path, "test", table, 2));
  scale = flag = 9;
  assert(settings_load(path, table, 2));
  assert(scale == 4 && flag == 1);

  file = fopen(path, "w");
  fputs("; scale = 2\nscale = 3\n  flag=0   ; off\nbogus = 1\nscale\n", file);
  fclose(file);
  assert(settings_load(path, table, 2));
  assert(scale == 4 && flag == 0);      /* 3 is not allowed; the commented-out line is skipped */
  assert(!settings_load("build/no_such_file.ini", table, 2));
  remove(path);
  puts("settings: ok");
  return 0;
}
#endif
