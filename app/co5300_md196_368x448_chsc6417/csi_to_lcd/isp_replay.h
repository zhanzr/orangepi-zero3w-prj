/* isp_replay.h - recorded AWISPdemo ISP parameter table (see .c) */
#ifndef ISP_REPLAY_H
#define ISP_REPLAY_H

#include <stddef.h>

typedef struct
{
    unsigned req;
    unsigned len;
    unsigned char data[36];
} isp_rec_t;

extern const isp_rec_t ISP_REPLAY_ISP[];
extern const unsigned ISP_REPLAY_ISP_N;
extern const isp_rec_t ISP_REPLAY_H3A[];
extern const unsigned ISP_REPLAY_H3A_N;

/* re-program the ISP with the recorded parameter blocks */
int isp_replay_apply(void);

/* consume pending ISP events (call once per capture iteration;
 * frame-done arrives as an ISP event on this BSP) */
int isp_events_fetch(void);

#endif /* ISP_REPLAY_H */
