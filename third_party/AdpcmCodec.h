#ifndef _ADPCM_CODEC_H
#define _ADPCM_CODEC_H

typedef struct adpcm_state
{
    short valprev; /* Previous output value */
    char index; /* Index into stepsize table */
} adpcm_state_t;

int adpcm_encoder(short *indata, char *outdata, int len, adpcm_state_t *state);
int adpcm_decoder(char *indata, short *outdata, int len, adpcm_state_t *state);
#endif

