/*
 * ml-msp.h - Betaflight MSP client for the air-unit FC UART.
 */
#ifndef ML_MSP_H
#define ML_MSP_H

#include <stddef.h>
#include <stdint.h>

#define ML_MSP_DEFAULT_TTY "/dev/ttyS1"
#define ML_MSP_FRESH_MS    15000

struct ml_msp_fc_status {
    uint8_t arm_flag;
    uint16_t mah_drawn_x10;
    uint16_t voltage_mv;
    long last_rx_ms;
    long last_battery_ms;
};

typedef void (*ml_msp_canvas_cb)(const uint8_t *canvas, size_t len, void *ctx);

struct ml_msp {
    int fd;
    uint8_t rx[512];
    size_t rx_len;
    long next_status_ms;
    long next_battery_ms;
    uint32_t canvas_seq;
    uint8_t records[384];
    size_t records_len;
    uint8_t record_count;
    struct ml_msp_fc_status status;
    ml_msp_canvas_cb canvas_cb;
    void *canvas_ctx;
};

void ml_msp_init(struct ml_msp *msp, ml_msp_canvas_cb canvas_cb, void *canvas_ctx);
int ml_msp_open(struct ml_msp *msp, const char *path);
void ml_msp_close(struct ml_msp *msp);
void ml_msp_service(struct ml_msp *msp, long now_ms);
int ml_msp_fc_voltage_mv(const struct ml_msp *msp, long now_ms);
int ml_msp_fc_present(const struct ml_msp *msp, long now_ms);

#endif /* ML_MSP_H */
