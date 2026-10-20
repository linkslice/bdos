/* SUNSET.C - a C program that changes the desktop it's running on.
 * Run it in a shell window on the desktop. */
#include <stdio.h>
#include <desk.h>

int mix(int from, int to, int step, int steps)
{
	int r = ((from >> 16) & 255) + (((to >> 16) & 255) - ((from >> 16) & 255)) * step / steps;
	int g = ((from >> 8) & 255) + (((to >> 8) & 255) - ((from >> 8) & 255)) * step / steps;
	int b = (from & 255) + ((to & 255) - (from & 255)) * step / steps;
	return r << 16 | g << 8 | b;
}

int main(void)
{
	int sky[] = { 0x1d3a8a, 0x7a3f8f, 0xd0603a, 0x3a1d2a };
	int i, step, colour = sky[0];

	for (i = 0; i < 3; i++)
		for (step = 0; step <= 20; step++) {
			colour = mix(sky[i], sky[i + 1], step, 20);
			desk_color(colour);
			os_sleep(25);
		}
	printf("desktop is now #%06x\n", colour);
	return 0;
}
