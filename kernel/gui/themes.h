/* themes.h - desktop themes: window colours, desktop colour and pattern */
#ifndef BDOS_THEMES_H
#define BDOS_THEMES_H

#include <stdint.h>

#define THEMES_DIR "C:\\BDOS\\THEMES"

struct theme_file {
	char name[48];
	uint32_t face, active, inactive, title_text, menu_text, select, workspace;
	uint32_t desktop, pattern_ink;
	uint8_t pattern[8];
	int has_pattern, timeofday;
};

int themes_list(char names[][48], int max);			/* sorted */
int theme_load(const char *name, struct theme_file *t);		/* 0 or E_ */
int theme_save(const struct theme_file *t);
void theme_current(struct theme_file *t);			/* what's showing now */
int theme_is_builtin(const char *name);
uint32_t time_of_day_colour(int minutes);			/* 0..1439 */

#endif
