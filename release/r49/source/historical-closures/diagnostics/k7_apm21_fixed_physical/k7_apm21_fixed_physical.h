#ifndef K7_APM21_FIXED_PHYSICAL_H
#define K7_APM21_FIXED_PHYSICAL_H
#if defined(K7_APM21_FIXED_PHYSICAL) && K7_APM21_FIXED_PHYSICAL
/* Explicit private diagnostic. Starts an existing fresh hardware session only
 * after constructing APM on its one256KiB worker. No model/VAD/ASR attachment,
 * natural interruption action, measured AEC alignment or verified live profile.
 * Fixed ROM speech repeats in one16second TX; archives the first15seconds of
 * genuinely paired MIC/reference/raw API-Y. No filler or cross-epoch restart.
 */
int k7_apm21_fixed_physical_command(int argc,char **argv);
#endif
#endif
