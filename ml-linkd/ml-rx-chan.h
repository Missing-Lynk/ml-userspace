/*
 * ml-rx-chan.h - the RX role's RF channel: which one the goggle is tuned to, what the band offers,
 * and the link measurements Get1V1Info carries.
 */
#ifndef ML_RX_CHAN_H
#define ML_RX_CHAN_H

#include <stdint.h>

/* Reply handlers, called from the reader thread. */
void rx_chan_on_1v1(const uint8_t *payload, int plen);
void rx_chan_on_scan_result(const uint8_t *payload, int plen);

/* HUD requests, queued from the UDP thread: the selects and the sweep must be issued by the
 * bb-socket TX thread only, or they race the steady poll and get lost. */
void rx_chan_request_select(unsigned chnidx);
void rx_chan_request_scan(void);
void rx_chan_request_mcs(unsigned mcs);   /* MLM_MCS_AUTO hands the rate back to the chip */

/* bb-socket TX thread (main) only. rx_chan_open reads the band and sets the channel once, during
 * OPEN; it sets none if the chip does not answer. */
void rx_chan_open(uint8_t *frame, uint32_t *seq_link);
void rx_chan_service(uint8_t *frame, uint32_t *seq_link);
void rx_chan_table_publish(void);

/* Snapshots for the OSD publish. */
int rx_chan_index(void);
uint32_t rx_chan_valid_bmp(void);         /* band's valid-channel mask, 0 = not read back yet */
int rx_chan_snr_db(void);
int rx_chan_distance_m(void);
int rx_chan_throughput_kbps(void);

#endif /* ML_RX_CHAN_H */
