/***************************************************************************
 * Colorfly C10 board audio hooks used by the PCM and jack drivers
 *
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/

#ifndef CODEC_COLORFLY_C10_H
#define CODEC_COLORFLY_C10_H

void c10_audio_start_playback(void);
void c10_audio_output_started(void);
void c10_audio_dma_buffer_complete(void);
void c10_audio_stop_playback(void);
void c10_audio_jack_changed(void);

#endif
