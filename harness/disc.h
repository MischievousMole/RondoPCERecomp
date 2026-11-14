#ifndef DISC_H
#define DISC_H
#include <stddef.h>
struct pce;
int disc_open(struct pce *m, const char *cue_path, char *err, size_t errn);
#endif
