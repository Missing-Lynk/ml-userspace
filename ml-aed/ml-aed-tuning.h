/* ml-aed's operating parameters, loaded at startup from the sensor tuning blob. */
#ifndef ML_AED_TUNING_H
#define ML_AED_TUNING_H

#include <stdint.h>
#include <stddef.h>

#include "ml-aed-blob.h"

struct mlaed_exp_entry {
    uint32_t gain_q8;
    uint32_t line_count;
};

struct ae_tuning {
    float tolerance;            /* blob 0xba600, luma either side that counts as settled */
    float damping;              /* blob 0xba5fc / 256 */
    float log_ladder;           /* blob 0xba620 */

    struct {
        int index;
        int target;
    } curve[MLAED_TARGET_CURVE_COUNT];

    struct mlaed_exp_entry *table;
    size_t table_len;
    int index_min;
    int index_max;
};

/* Returns 0 or negative errno; -EINVAL means the file is not MLAED_TUNING_SIZE bytes. */
int ae_tuning_load(struct ae_tuning *t, const char *path);
void ae_tuning_free(struct ae_tuning *t);

#endif /* ML_AED_TUNING_H */
