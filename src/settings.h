/* Settings kept in a text file as "name = value" lines: an INI file without sections, where
   ';' starts a comment. */
#ifndef VRDE_SETTINGS_H
#define VRDE_SETTINGS_H

typedef struct {
  const char *name;
  int *value;
  unsigned allowed;      /* bit n set: n is a valid value (0 to 31) */
  const char *comment;   /* written after the value */
} setting;

/* Read path into the settings it names. Lines that are not "name = number", unknown names
   and values that are not allowed are ignored. Returns 0 when the file cannot be read. */
int settings_load(const char *path, const setting *settings, int count);

/* Write all the settings to path after a comment line; returns 0 when it cannot be written */
int settings_save(const char *path, const char *header, const setting *settings, int count);

#endif
