/* DRAW.C - a window to draw in, from C (run it on the desktop)
 *
 * Functions this program doesn't define come from the SYS table, the same
 * one BASIC uses: win_open() is SYS "Win_Open", desk_color() is
 * SYS "Desk_Color". Type HELP SYS for the list.
 */
#include <stdio.h>
#include <math.h>
#include <desk.h>

#define W 400
#define H 300

struct point { int x, y; };

static void spokes(Window w, struct point centre, int r, int colour)
{
	double a;
	for (a = 0; a < 2 * M_PI; a += M_PI / 24)
		win_line(w, centre.x, centre.y, centre.x + (int)(r * cos(a)),
		    centre.y + (int)(r * sin(a)), colour);
}

int main(void)
{
	Window w = win_open("Drawing from C", -1, -1, W, H);
	struct point centre = { 300, 200 };
	int i, e;

	win_clear(w, 0xFFFFFF);
	for (i = 0; i < 16; i++)
		win_rect(w, 20 + i * 10, 20 + i * 8, 120, 80,
		    (i * 16) << 16 | (255 - i * 16) << 8 | 128);
	spokes(w, centre, 80, 0x1D3A8A);
	win_text(w, 20, 270, "Click to paint, close the window to stop", 0x000000);

	while ((e = win_event(w)) != -1) {
		if (e == 2)
			win_rect(w, win_x(w) - 3, win_y(w) - 3, 6, 6, 0xD03030);
		else if (e == 0)
			os_sleep(10);
	}
	win_close(w);
	return 0;
}
