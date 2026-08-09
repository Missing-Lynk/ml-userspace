/*
 * ml-rx-reader.h - the RX role's bb-socket reader thread.
 */
#ifndef ML_RX_READER_H
#define ML_RX_READER_H

/* Deframes everything the chip sends on /dev/artosyn_sdio and hands each recognised reply to the
 * module that owns it. Runs until g_run clears. */
void *rx_reader_thread(void *arg);

#endif /* ML_RX_READER_H */
