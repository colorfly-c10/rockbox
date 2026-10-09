/***************************************************************************
 * Colorfly C10 DAC/relay lifecycle hooks for the shared PCM driver
 * Copyright (C) 2026 by Anton Bambura
 ****************************************************************************/
#ifndef PCM_TARGET_H
#define PCM_TARGET_H
#include "codec-colorfly-c10.h"
#define PCM_DMA_BUFFER_COMPLETE c10_audio_dma_buffer_complete
#define PCM_DMA_START c10_audio_start_playback
#define PCM_DMA_STOP c10_audio_stop_playback
#define PCM_OUTPUT_STARTED c10_audio_output_started
#endif
