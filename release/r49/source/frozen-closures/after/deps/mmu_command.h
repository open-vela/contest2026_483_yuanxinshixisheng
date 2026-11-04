#ifndef K7SOUND_MMU_COMMAND_H
#define K7SOUND_MMU_COMMAND_H
#include <pthread.h>
int k7sound_mmu_command(int argc,char **argv,pthread_mutex_t *audio_owner);
#endif
