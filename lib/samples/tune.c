/*
 * TUNE.EXE - a little synthesizer: plays a melody with a bass line, from
 * a native program, through whatever sound card bdos found.
 */
#include <stdio.h>
#include <math.h>
#include <bdos.h>

#define RATE 22050

static double note_hz(int n) { return 440.0 * pow(2.0, (n - 69) / 12.0); }

int main(void)
{
	/* MIDI note numbers, eighth notes; 0 is a rest */
	static const int melody[] = { 72, 74, 76, 77, 79, 0, 79, 0, 81, 79, 77, 76, 74, 0, 72, 0,
				      76, 77, 79, 81, 83, 0, 84, 0, 83, 81, 79, 77, 76, 74, 72, 0 };
	static const int bass[] = { 48, 48, 55, 55, 53, 53, 48, 48 };
	int s = bdos_audio_open(RATE);
	if (s < 0) {
		printf("No sound stream.\n");
		return 1;
	}
	printf("Playing a tune (%d notes)...\n", (int)(sizeof melody / sizeof melody[0]));
	static short buf[RATE / 4 * 2];
	double ph1 = 0, ph2 = 0;
	int n = (int)(sizeof melody / sizeof melody[0]);
	for (int i = 0; i < n; i++) {
		int len = RATE / 4;
		double f1 = melody[i] ? note_hz(melody[i]) : 0, f2 = note_hz(bass[i / 4 % 8]);
		for (int k = 0; k < len; k++) {
			double env = melody[i] ? exp(-3.0 * k / len) : 0;
			double v = 0.35 * env * (sin(ph1) + 0.25 * sin(2 * ph1)) + 0.2 * (ph2 < M_PI ? 1 : -1) * exp(-1.5 * k / len);
			ph1 += 2 * M_PI * f1 / RATE;
			ph2 += 2 * M_PI * f2 / RATE;
			if (ph1 > 2 * M_PI) ph1 -= 2 * M_PI;
			if (ph2 > 2 * M_PI) ph2 -= 2 * M_PI;
			buf[k * 2] = (short)(v * 32000);
			buf[k * 2 + 1] = (short)(v * 26000);
		}
		bdos_audio_write(s, buf, len, 2);
	}
	bdos_audio_close(s);
	printf("Done.\n");
	return 0;
}
